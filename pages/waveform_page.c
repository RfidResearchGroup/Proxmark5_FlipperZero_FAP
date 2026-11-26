#include <dialogs/dialogs.h>
#include <furi.h>
#include <gui/elements.h>
#include <storage/storage.h>
#include "../readers/bigbuf.h"
#include "../readers/lf_capture.h"
#include "waveform_page.h"
#include "proxmark5_alloc.h"
#include "proxmark5_frame.h"

#define LF_CAPTURE_COUNT 8192U
#define WAVEFORM_EXT ".pm3"
#define PLOT_WIDTH 118U
#define PLOT_TOP 15
#define PLOT_HEIGHT 34

static int capture_lf(uint8_t* samples) {
    int status = lf_capture_samples(LF_CAPTURE_COUNT);
    return status == PM3_SUCCESS ? bigbuf_download(samples, 0, LF_CAPTURE_COUNT) : status;
}

typedef struct {
    const char* title;
    const char* description;
    const char* directory;
    const char* prefix;
    uint16_t count;
    uint16_t buffer_size;
    int (*capture)(uint8_t* samples);
} WaveformInfo;

static const WaveformInfo waveform_info[] = {
    [WaveformLf] = {
        .title = "LF Capture",
        .description = "125 kHz LF field",
        .directory = APP_DATA_PATH("captures"),
        .prefix = "LF",
        .count = LF_CAPTURE_COUNT,
        .buffer_size = LF_CAPTURE_COUNT,
        .capture = capture_lf,
    },
};

typedef enum {
    CaptureIdle,
    CaptureReading,
    CaptureOpening,
    CaptureSaving,
    CaptureReady,
    CaptureReadFailed,
    CaptureRestoreFailed,
    CaptureOpenFailed,
} CaptureState;

typedef struct {
    CaptureState state;
    const WaveformInfo* info;
    const uint8_t* samples;
    uint16_t start;
    uint16_t span;
    bool can_save;
    bool save_failed;
} CaptureModel;

struct WaveformPage {
    View* view;
    FuriThread* worker;
    FuriString* file;
    uint8_t* samples;
    uint32_t previous_view;
};

static bool write_samples(File* file, const uint8_t* samples, size_t count) {
    char lines[64 * 5 + 1];
    for(size_t start = 0; start < count; start += 64) {
        size_t used = 0;
        for(size_t i = start; i < MIN(start + 64, count); i++) {
            used += snprintf(lines + used, sizeof(lines) - used, "%d\n", samples[i] - 128);
        }
        if(storage_file_write(file, lines, used) != used) return false;
    }
    return storage_file_sync(file);
}

static bool save_capture(const WaveformInfo* info, const uint8_t* samples) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FuriString* path = furi_string_alloc_set(info->directory);
    FuriString* name = furi_string_alloc();
    storage_common_resolve_path_and_ensure_app_directory(storage, path);
    File* file = storage_file_alloc(storage);
    bool saved = false;
    if(storage_simply_mkdir(storage, furi_string_get_cstr(path))) {
        storage_get_next_filename(
            storage, furi_string_get_cstr(path), info->prefix, WAVEFORM_EXT, name, 24);
        furi_string_cat_printf(path, "/%s" WAVEFORM_EXT, furi_string_get_cstr(name));
        const char* filename = furi_string_get_cstr(path);
        if(storage_file_open(file, filename, FSAM_WRITE, FSOM_CREATE_NEW)) {
            bool written = write_samples(file, samples, info->count);
            bool closed = storage_file_close(file);
            saved = written && closed;
            if(!saved) storage_common_remove(storage, filename);
        }
    }
    storage_file_free(file);
    furi_string_free(name);
    furi_string_free(path);
    furi_record_close(RECORD_STORAGE);
    return saved;
}

static bool read_samples(File* file, uint8_t* samples, size_t expected_count) {
    char block[256];
    char line[6];
    size_t used = 0;
    size_t count = 0;
    for(size_t size; (size = storage_file_read(file, block, sizeof(block))) != 0;) {
        for(size_t i = 0; i < size; i++) {
            char c = block[i];
            if(c == '\n') {
                if(used && line[used - 1] == '\r') used--;
                line[used] = '\0';
                char* end;
                long value = strtol(line, &end, 10);
                if(!used || *end || value < -128 || value > 127 || count == expected_count) {
                    return false;
                }
                samples[count++] = value + 128;
                used = 0;
            } else {
                bool digit = c >= '0' && c <= '9';
                if(used == sizeof(line) - 1 || (used && line[used - 1] == '\r') ||
                   (!digit && c != '\r' && (c != '-' || used))) {
                    return false;
                }
                line[used++] = c;
            }
        }
    }
    return storage_file_get_error(file) == FSE_OK && !used && count == expected_count;
}

static bool open_capture(const char* path, uint8_t* samples, size_t count) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    bool opened = storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING) &&
                  read_samples(file, samples, count);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return opened;
}

static void draw_plot(Canvas* canvas, const CaptureModel* model) {
    char label[24];
    snprintf(label, sizeof(label), "%u/%u", model->span, model->info->count);
    canvas_draw_str_aligned(canvas, 124, 11, AlignRight, AlignBottom, label);
    canvas_draw_frame(canvas, 4, PLOT_TOP, PLOT_WIDTH + 2, PLOT_HEIGHT + 2);

    for(size_t x = 0; x < PLOT_WIDTH; x++) {
        size_t start = model->start + x * model->span / PLOT_WIDTH;
        size_t end = model->start + (x + 1) * model->span / PLOT_WIDTH;
        uint8_t low = model->samples[start];
        uint8_t high = low;
        for(size_t i = start + 1; i < end; i++) {
            low = MIN(low, model->samples[i]);
            high = MAX(high, model->samples[i]);
        }
        int top = PLOT_TOP + 1 + (255 - high) * (PLOT_HEIGHT - 1) / 255;
        int bottom = PLOT_TOP + 1 + (255 - low) * (PLOT_HEIGHT - 1) / 255;
        canvas_draw_line(canvas, 5 + x, top, 5 + x, bottom);
    }
    if(model->start) elements_button_left(canvas, "Pan");
    if(model->start + model->span < model->info->count) elements_button_right(canvas, "Pan");
    if(model->can_save) {
        elements_button_center(canvas, model->save_failed ? "Retry save" : "Save");
    } else {
        canvas_draw_str_aligned(canvas, 64, 62, AlignCenter, AlignBottom, "Saved");
    }
}

static void capture_draw(Canvas* canvas, void* context) {
    const CaptureModel* model = context;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 4, 11, model->info->title);
    canvas_set_font(canvas, FontSecondary);

    char count[24];
    switch(model->state) {
    case CaptureIdle:
        canvas_draw_str(canvas, 4, 28, model->info->description);
        snprintf(count, sizeof(count), "%u samples, 8-bit", model->info->count);
        canvas_draw_str(canvas, 4, 41, count);
        elements_button_center(canvas, "Capture");
        break;
    case CaptureReading:
        canvas_draw_str(canvas, 4, 30, "Capturing...");
        break;
    case CaptureOpening:
        canvas_draw_str(canvas, 4, 30, "Opening...");
        break;
    case CaptureSaving:
        canvas_draw_str(canvas, 4, 30, "Saving...");
        break;
    case CaptureReady:
        draw_plot(canvas, model);
        break;
    case CaptureReadFailed:
        canvas_draw_str(canvas, 4, 30, "Capture failed");
        elements_button_center(canvas, "Retry");
        break;
    case CaptureRestoreFailed:
        canvas_draw_str(canvas, 4, 27, "LF settings may have");
        canvas_draw_str(canvas, 4, 40, "changed. Check on PM5.");
        break;
    case CaptureOpenFailed:
        canvas_draw_str(canvas, 4, 27, "Cannot open capture");
        snprintf(count, sizeof(count), "Expected %u samples", model->info->count);
        canvas_draw_str(canvas, 4, 40, count);
        break;
    }
}

static int32_t capture_worker(void* context) {
    WaveformPage* page = context;
    CaptureModel* model = view_get_model(page->view);
    CaptureState job = model->state;
    const WaveformInfo* info = model->info;
    view_commit_model(page->view, false);

    CaptureState state;
    bool saved = false;
    if(!page->samples) page->samples = proxmark5_try_alloc(info->buffer_size);
    if(job == CaptureReading) {
        int status = PM3_EMALLOC;
        if(page->samples) {
            proxmark5_frame_begin_operation(NULL);
            status = info->capture(page->samples);
            proxmark5_frame_end_operation();
        }
        if(status == PM3_SUCCESS) {
            state = CaptureReady;
        } else if(status == LfCaptureRestoreFailed) {
            state = CaptureRestoreFailed;
        } else {
            state = CaptureReadFailed;
        }
    } else if(job == CaptureOpening) {
        bool opened = page->samples &&
                      open_capture(furi_string_get_cstr(page->file), page->samples, info->count);
        state = opened ? CaptureReady : CaptureOpenFailed;
    } else {
        saved = save_capture(info, page->samples);
        state = CaptureReady;
    }

    model = view_get_model(page->view);
    model->state = state;
    model->samples = page->samples;
    model->can_save = state == CaptureReady &&
                      (job == CaptureReading || (job == CaptureSaving && !saved));
    model->save_failed = job == CaptureSaving && !saved;
    view_commit_model(page->view, true);
    return 0;
}

static void start_job(WaveformPage* page, CaptureState job) {
    waveform_page_wait(page);
    CaptureModel* model = view_get_model(page->view);
    model->state = job;
    view_commit_model(page->view, true);
    page->worker = furi_thread_alloc_ex("PM5Capture", 4096, capture_worker, page);
    furi_thread_start(page->worker);
}

static bool capture_input(InputEvent* event, void* context) {
    WaveformPage* page = context;
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return false;

    CaptureModel* model = view_get_model(page->view);
    CaptureState state = model->state;
    if(state == CaptureReading || state == CaptureOpening || state == CaptureSaving) {
        view_commit_model(page->view, false);
        return true;
    }
    if(state == CaptureReady && event->key != InputKeyOk && event->key != InputKeyBack) {
        uint16_t span = model->span;
        uint16_t start = model->start;
        if(event->key == InputKeyUp && span / 2 >= PLOT_WIDTH) {
            span /= 2;
            start += span / 2;
        } else if(event->key == InputKeyDown && span < model->info->count) {
            start = start > span / 2 ? start - span / 2 : 0;
            span = MIN(span * 2, model->info->count);
        } else if(event->key == InputKeyLeft) {
            start = start > span / 4 ? start - span / 4 : 0;
        } else if(event->key == InputKeyRight) {
            start += span / 4;
        }
        model->span = span;
        model->start = MIN(start, model->info->count - span);
        view_commit_model(page->view, true);
        return true;
    }
    bool can_save = model->can_save;
    view_commit_model(page->view, false);
    if(event->key != InputKeyOk || event->type != InputTypeShort) return false;
    if(state == CaptureIdle || state == CaptureReadFailed) start_job(page, CaptureReading);
    if(state == CaptureReady && can_save) start_job(page, CaptureSaving);
    return true;
}

static void capture_exit(void* context) {
    WaveformPage* page = context;
    waveform_page_wait(page);
    CaptureModel* model = view_get_model(page->view);
    model->state = CaptureIdle;
    model->samples = NULL;
    free(page->samples);
    page->samples = NULL;
    view_commit_model(page->view, false);
}

WaveformPage* waveform_page_create(void) {
    WaveformPage* page = calloc(1, sizeof(WaveformPage));
    page->view = view_alloc();
    page->file = furi_string_alloc();
    view_set_context(page->view, page);
    view_set_draw_callback(page->view, capture_draw);
    view_set_input_callback(page->view, capture_input);
    view_set_exit_callback(page->view, capture_exit);
    view_allocate_model(page->view, ViewModelTypeLocking, sizeof(CaptureModel));
    waveform_page_reset(page, WaveformLf, 0);
    return page;
}

void waveform_page_reset(WaveformPage* page, WaveformBand band, uint32_t previous_view) {
    page->previous_view = previous_view;
    CaptureModel* model = view_get_model(page->view);
    const WaveformInfo* info = &waveform_info[band];
    *model = (CaptureModel){.info = info, .span = info->count};
    view_commit_model(page->view, true);
}

bool waveform_page_browse(WaveformPage* page, WaveformBand band, uint32_t previous_view) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FuriString* dir = furi_string_alloc_set(waveform_info[band].directory);
    storage_common_resolve_path_and_ensure_app_directory(storage, dir);
    bool exists = storage_simply_mkdir(storage, furi_string_get_cstr(dir));
    furi_record_close(RECORD_STORAGE);

    DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
    bool selected = false;
    if(exists) {
        DialogsFileBrowserOptions options;
        dialog_file_browser_set_basic_options(&options, WAVEFORM_EXT, NULL);
        options.base_path = furi_string_get_cstr(dir);
        selected = dialog_file_browser_show(dialogs, page->file, dir, &options);
    } else {
        dialog_message_show_storage_error(dialogs, "Cannot open captures");
    }
    furi_record_close(RECORD_DIALOGS);
    furi_string_free(dir);

    if(selected) {
        waveform_page_reset(page, band, previous_view);
        start_job(page, CaptureOpening);
    }
    return selected;
}

uint32_t waveform_page_previous(void* context) {
    WaveformPage* page = context;
    return page->previous_view;
}

View* waveform_page_get_view(WaveformPage* page) {
    return page->view;
}

void waveform_page_wait(WaveformPage* page) {
    if(!page->worker) return;
    furi_thread_join(page->worker);
    furi_thread_free(page->worker);
    page->worker = NULL;
}

void waveform_page_free(WaveformPage* page) {
    waveform_page_wait(page);
    if(page->samples) free(page->samples);
    furi_string_free(page->file);
    view_free(page->view);
    free(page);
}
