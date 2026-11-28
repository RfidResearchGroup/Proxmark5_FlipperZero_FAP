#include <dialogs/dialogs.h>
#include <fmps_cxt_icons.h>
#include <furi.h>
#include <gui/elements.h>
#include <storage/storage.h>
#include "../readers/bigbuf.h"
#include "../readers/lf_capture.h"
#include "../readers/hf_waveform.h"
#include "saved_file_browser.h"
#include "waveform_page.h"
#include "proxmark5_alloc.h"
#include "proxmark5_frame.h"

#define LF_CAPTURE_COUNT 16384U
#define LF_SHORT_CAPTURE_COUNT 8192U
#define WAVEFORM_EXT ".pm3"
#define PLOT_WIDTH 118U
#define PLOT_TOP 15
#define PLOT_HEIGHT 34

static int capture_lf(uint8_t* samples, size_t count) {
    int status = lf_capture_samples(count);
    return status == PM3_SUCCESS ? bigbuf_download(samples, 0, count) : status;
}

typedef struct {
    const char* title;
    const char* description;
    const char* directory;
    const char* prefix;
    uint16_t count;
} WaveformInfo;

static const WaveformInfo waveform_info[] = {
    [WaveformLf] = {
        .title = "LF Capture",
        .description = "125 kHz LF field",
        .directory = APP_DATA_PATH("captures"),
        .prefix = "LF",
        .count = LF_CAPTURE_COUNT,
    },
    [WaveformHf] = {
        .title = "HF Plot",
        .description = "ISO 14443A poll",
        .directory = APP_DATA_PATH("hf-captures"),
        .prefix = "HF",
        .count = HF_WAVEFORM_COUNT,
    },
};

typedef enum {
    CaptureIdle,
    CaptureReading,
    CaptureOpening,
    CaptureSaving,
    CaptureReady,
    CaptureReadFailed,
    CaptureNoMemory,
    CaptureIncomplete,
    CaptureRestoreFailed,
    CaptureOpenFailed,
} CaptureState;

typedef struct {
    CaptureState state;
    WaveformBand band;
    const WaveformInfo* info;
    const uint8_t* samples;
    uint16_t count;
    uint16_t start;
    uint16_t span;
    bool can_save;
    bool save_failed;
    bool low_memory;
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

static bool save_capture(const WaveformInfo* info, const uint8_t* samples, size_t count) {
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
            bool written = write_samples(file, samples, count);
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

static size_t read_samples(File* file, uint8_t* samples, size_t capacity) {
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
                if(!used || *end || value < -128 || value > 127 || count == capacity) {
                    return 0;
                }
                samples[count++] = value + 128;
                used = 0;
            } else {
                bool digit = c >= '0' && c <= '9';
                if(used == sizeof(line) - 1 || (used && line[used - 1] == '\r') ||
                   (!digit && c != '\r' && (c != '-' || used))) {
                    return 0;
                }
                line[used++] = c;
            }
        }
    }
    return storage_file_get_error(file) == FSE_OK && !used ? count : 0;
}

static size_t open_capture(const char* path, uint8_t* samples, size_t capacity) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    size_t count = 0;
    if(storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        count = read_samples(file, samples, capacity);
    }
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return count;
}

static void draw_plot(Canvas* canvas, const CaptureModel* model) {
    char label[24];
    snprintf(label, sizeof(label), "%u/%u", model->span, model->count);
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
    if(model->start + model->span < model->count) elements_button_right(canvas, "Pan");
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
    const char* title =
        model->state == CaptureReady && model->low_memory ? "LF: low RAM" : model->info->title;
    canvas_draw_str(canvas, 4, 11, title);
    canvas_set_font(canvas, FontSecondary);

    char count[24];
    switch(model->state) {
    case CaptureIdle:
        canvas_draw_str(canvas, 4, 28, model->info->description);
        snprintf(count, sizeof(count), "%u samples, 8-bit", model->count);
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
    case CaptureNoMemory:
        canvas_draw_str(
            canvas, 4, 30, model->state == CaptureNoMemory ? "Not enough memory" : "Capture failed");
        elements_button_center(canvas, "Retry");
        break;
    case CaptureIncomplete:
        canvas_draw_str(canvas, 4, 30, "Capture incomplete");
        elements_button_center(canvas, "Retry");
        break;
    case CaptureRestoreFailed:
        canvas_draw_str(canvas, 4, 27, "LF settings may have");
        canvas_draw_str(canvas, 4, 40, "changed. Check on PM5.");
        break;
    case CaptureOpenFailed:
        canvas_draw_str(canvas, 4, 27, "Cannot open capture");
        if(model->band == WaveformLf) {
            snprintf(count, sizeof(count), "8192 or 16384 samples");
        } else {
            snprintf(count, sizeof(count), "Expected %u samples", model->count);
        }
        canvas_draw_str(canvas, 4, 40, count);
        break;
    }
}

static int32_t capture_worker(void* context) {
    WaveformPage* page = context;
    CaptureModel* model = view_get_model(page->view);
    CaptureState job = model->state;
    const WaveformInfo* info = model->info;
    const bool lf = model->band == WaveformLf;
    size_t count = job == CaptureSaving ? model->count : info->count;
    view_commit_model(page->view, false);

    CaptureState state;
    bool saved = false;
    size_t capacity = count;
    if(job != CaptureSaving) {
        free(page->samples);
        page->samples = proxmark5_try_alloc(capacity);
        if(!page->samples && lf) {
            capacity = LF_SHORT_CAPTURE_COUNT;
            page->samples = proxmark5_try_alloc(capacity);
        }
    }
    if(job == CaptureReading) {
        int status = PM3_EMALLOC;
        if(page->samples) {
            count = capacity;
            proxmark5_frame_begin_operation(NULL);
            status = lf ? capture_lf(page->samples, count) : hf_waveform_capture(page->samples);
            proxmark5_frame_end_operation();
        }
        if(status == PM3_SUCCESS) {
            state = CaptureReady;
        } else if(status == PM3_EMALLOC) {
            state = CaptureNoMemory;
        } else if(status == LfCaptureRestoreFailed) {
            state = CaptureRestoreFailed;
        } else if(status == PM3_EPARTIAL) {
            state = CaptureIncomplete;
        } else {
            state = CaptureReadFailed;
        }
    } else if(job == CaptureOpening) {
        size_t length = page->samples ?
                            open_capture(furi_string_get_cstr(page->file), page->samples, capacity) :
                            0;
        state = CaptureOpenFailed;
        if(length == info->count || (lf && length == LF_SHORT_CAPTURE_COUNT)) {
            count = length;
            state = CaptureReady;
        }
    } else {
        saved = save_capture(info, page->samples, count);
        state = CaptureReady;
    }

    model = view_get_model(page->view);
    model->state = state;
    model->samples = page->samples;
    model->count = count;
    if(job != CaptureSaving) model->span = count;
    if(job == CaptureReading) {
        model->low_memory = lf && state == CaptureReady && count == LF_SHORT_CAPTURE_COUNT;
    }
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
    bool ready_to_capture = state == CaptureIdle || state == CaptureReadFailed ||
                            state == CaptureNoMemory || state == CaptureIncomplete;
    if(state == CaptureReady && event->key != InputKeyOk && event->key != InputKeyBack) {
        uint16_t span = model->span;
        uint16_t start = model->start;
        if(event->key == InputKeyUp && span / 2 >= PLOT_WIDTH) {
            span /= 2;
            start += span / 2;
        } else if(event->key == InputKeyDown && span < model->count) {
            start = start > span / 2 ? start - span / 2 : 0;
            span = MIN(span * 2, model->count);
        } else if(event->key == InputKeyLeft) {
            start = start > span / 4 ? start - span / 4 : 0;
        } else if(event->key == InputKeyRight) {
            start += span / 4;
        }
        model->span = span;
        model->start = MIN(start, model->count - span);
        view_commit_model(page->view, true);
        return true;
    }
    bool can_save = model->can_save;
    view_commit_model(page->view, false);
    if(event->key != InputKeyOk || event->type != InputTypeShort) return false;
    if(ready_to_capture) start_job(page, CaptureReading);
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
    *model = (CaptureModel){.band = band, .info = info, .count = info->count, .span = info->count};
    view_commit_model(page->view, true);
}

bool waveform_page_browse(WaveformPage* page, WaveformBand band, uint32_t previous_view) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FuriString* dir = furi_string_alloc_set(waveform_info[band].directory);
    storage_common_resolve_path_and_ensure_app_directory(storage, dir);
    bool exists = storage_simply_mkdir(storage, furi_string_get_cstr(dir));
    DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
    bool selected = false;
    if(exists) {
        selected = saved_file_browse(
            storage,
            dialogs,
            page->file,
            furi_string_get_cstr(dir),
            WAVEFORM_EXT,
            &I_pm5_tag_file_10px,
            NULL);
    } else {
        dialog_message_show_storage_error(dialogs, "Cannot open captures");
    }
    furi_record_close(RECORD_DIALOGS);
    furi_record_close(RECORD_STORAGE);
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
