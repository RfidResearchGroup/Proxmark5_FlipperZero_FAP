#include <dialogs/dialogs.h>
#include <furi.h>
#include <gui/elements.h>
#include <storage/storage.h>
#include "../readers/hf14a_trace.h"
#include "hf14a_trace_page.h"
#include "proxmark5_alloc.h"
#include "proxmark5_frame.h"

#define TRACE_DIR APP_DATA_PATH("hf-traces")
#define TRACE_EXT ".trace"
#define BYTES_PER_ROW 6U
#define ROWS_PER_PAGE 3U
#define BYTES_PER_PAGE (BYTES_PER_ROW * ROWS_PER_PAGE)

typedef enum {
    TraceCapture,
    TraceOpen,
    TraceSave,
    TraceReady,
    TraceCaptureError,
    TraceOpenError,
} TraceState;

typedef struct {
    TraceState state;
    int status;
    const uint8_t* data;
    size_t length;
    size_t offset;
    size_t frame;
    size_t frames;
    size_t first_byte;
    bool saved;
    bool save_failed;
} TraceModel;

struct Hf14aTracePage {
    View* view;
    FuriThread* worker;
    FuriString* path;
    uint8_t* data;
    uint32_t previous_view;
};

static FuriString* trace_directory(Storage* storage) {
    FuriString* path = furi_string_alloc_set(TRACE_DIR);
    storage_common_resolve_path_and_ensure_app_directory(storage, path);
    if(storage_simply_mkdir(storage, furi_string_get_cstr(path))) return path;
    furi_string_free(path);
    return NULL;
}

static bool trace_save(const uint8_t* data, size_t count) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FuriString* path = trace_directory(storage);
    bool saved = false;
    if(path) {
        FuriString* name = furi_string_alloc();
        storage_get_next_filename(storage, furi_string_get_cstr(path), "14A", TRACE_EXT, name, 24);
        furi_string_cat_printf(path, "/%s" TRACE_EXT, furi_string_get_cstr(name));
        furi_string_free(name);

        const char* filename = furi_string_get_cstr(path);
        File* file = storage_file_alloc(storage);
        if(storage_file_open(file, filename, FSAM_WRITE, FSOM_CREATE_NEW)) {
            bool written = storage_file_write(file, data, count) == count && storage_file_sync(file);
            saved = storage_file_close(file) && written;
            if(!saved) storage_common_remove(storage, filename);
        }
        storage_file_free(file);
        furi_string_free(path);
    }
    furi_record_close(RECORD_STORAGE);
    return saved;
}

static uint32_t trace_open(const char* path, uint8_t* data) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    uint32_t count = 0;
    if(storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        uint64_t size = storage_file_size(file);
        if(size <= HF14A_TRACE_CAPACITY && storage_file_read(file, data, size) == size) {
            count = size;
        }
    }
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return count;
}

static size_t trace_count(const uint8_t* data, size_t count) {
    Hf14aTraceFrame frame;
    size_t frames = 0;
    for(size_t offset = 0; offset < count; offset = frame.next) {
        if(!hf14a_trace_frame(data, count, offset, &frame)) return 0;
        frames++;
    }
    return frames;
}

static int32_t trace_worker(void* context) {
    Hf14aTracePage* page = context;
    TraceModel* model = view_get_model(page->view);
    TraceState job = model->state;
    size_t count = model->length;
    view_commit_model(page->view, false);

    if(job == TraceSave) {
        bool saved = trace_save(page->data, count);
        model = view_get_model(page->view);
        model->state = TraceReady;
        model->saved = saved;
        model->save_failed = !saved;
        view_commit_model(page->view, true);
        return 0;
    }

    uint32_t length = 0;
    int status = PM3_EMALLOC;
    if(!page->data) page->data = proxmark5_try_alloc(HF14A_TRACE_CAPACITY);
    if(page->data && job == TraceCapture) {
        proxmark5_frame_begin_operation(NULL);
        status = hf14a_trace_capture(page->data, &length);
        proxmark5_frame_end_operation();
        if(status != PM3_SUCCESS) length = 0;
    } else if(page->data) {
        length = trace_open(furi_string_get_cstr(page->path), page->data);
        status = length ? PM3_SUCCESS : PM3_EFILE;
    }

    size_t frames = trace_count(page->data, length);
    TraceState error = job == TraceOpen ? TraceOpenError : TraceCaptureError;
    model = view_get_model(page->view);
    *model = (TraceModel){
        .state = frames ? TraceReady : error,
        .data = page->data,
        .status = status,
        .length = length,
        .frames = frames,
        .saved = job == TraceOpen,
    };
    view_commit_model(page->view, true);
    return 0;
}

static void start_job(Hf14aTracePage* page, TraceState job) {
    hf14a_trace_page_wait(page);
    TraceModel* model = view_get_model(page->view);
    model->state = job;
    view_commit_model(page->view, true);
    page->worker = furi_thread_alloc_ex("PM5Trace", 4096, trace_worker, page);
    furi_thread_start(page->worker);
}

static void trace_draw(Canvas* canvas, void* context) {
    const TraceModel* model = context;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 4, 11, "14A Trace");
    canvas_set_font(canvas, FontSecondary);
    if(model->state != TraceReady) {
        const char* messages[] = {
            [TraceCapture] = "Capturing...",
            [TraceOpen] = "Opening...",
            [TraceSave] = "Saving...",
            [TraceCaptureError] = "Trace unavailable",
            [TraceOpenError] = "Cannot open trace",
        };
        canvas_draw_str(canvas, 4, 33, messages[model->state]);
        if(model->state == TraceCaptureError) {
            if(model->status == PM3_ETIMEOUT) {
                canvas_draw_str(canvas, 4, 44, "PM5 did not reply");
            } else if(model->status == PM3_ENODATA) {
                canvas_draw_str(canvas, 4, 44, "No frames captured");
            }
            elements_button_center(canvas, "Retry");
        }
        return;
    }

    Hf14aTraceFrame first;
    Hf14aTraceFrame frame;
    if(!hf14a_trace_frame(model->data, model->length, 0, &first) ||
       !hf14a_trace_frame(model->data, model->length, model->offset, &frame)) return;

    char line[26];
    snprintf(line, sizeof(line), "%zu/%zu", model->frame + 1, model->frames);
    canvas_draw_str_aligned(canvas, 124, 11, AlignRight, AlignBottom, line);
    // ISO 14443A trace timestamps are in 13.56 MHz carrier periods.
    uint32_t ticks = frame.timestamp - first.timestamp;
    uint32_t us = ((uint64_t)ticks * 100 + 678) / 1356;
    snprintf(line, sizeof(line), "%s +%lu us", frame.from_tag ? "Tag" : "Rdr", (unsigned long)us);
    canvas_draw_str(canvas, 4, 21, line);
    snprintf(line, sizeof(line), "%u us", (frame.duration * 100U + 678) / 1356);
    canvas_draw_str_aligned(canvas, 124, 21, AlignRight, AlignBottom, line);

    canvas_set_font(canvas, FontKeyboard);
    size_t byte = model->first_byte;
    for(size_t row = 0; row < ROWS_PER_PAGE && byte < frame.length; row++) {
        size_t columns = MIN(BYTES_PER_ROW, frame.length - byte);
        for(size_t col = 0; col < columns; col++) {
            snprintf(line + col * 3, sizeof(line) - col * 3, "%02X ", frame.data[byte++]);
        }
        canvas_draw_str(canvas, 4, 31 + 9 * row, line);
    }
    if(model->first_byte) elements_button_left(canvas, "Prev");
    if(byte < frame.length) elements_button_right(canvas, "More");
    if(!model->saved) elements_button_center(canvas, model->save_failed ? "Retry save" : "Save");
}

static size_t previous_frame(const TraceModel* model) {
    size_t offset = 0;
    Hf14aTraceFrame frame;
    while(hf14a_trace_frame(model->data, model->length, offset, &frame)) {
        if(frame.next >= model->offset) break;
        offset = frame.next;
    }
    return offset;
}

static bool trace_input(InputEvent* event, void* context) {
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return false;
    Hf14aTracePage* page = context;
    TraceModel* model = view_get_model(page->view);
    TraceState state = model->state;
    bool confirm = event->key == InputKeyOk && event->type == InputTypeShort;
    bool save = false;
    bool handled = state != TraceCaptureError && state != TraceOpenError;
    if(state == TraceReady) {
        Hf14aTraceFrame frame;
        handled = event->key != InputKeyBack;
        if(hf14a_trace_frame(model->data, model->length, model->offset, &frame)) {
            if(event->key == InputKeyUp && model->frame) {
                model->offset = previous_frame(model);
                model->frame--;
                model->first_byte = 0;
            } else if(event->key == InputKeyDown && model->frame + 1 < model->frames) {
                model->offset = frame.next;
                model->frame++;
                model->first_byte = 0;
            } else if(event->key == InputKeyLeft && model->first_byte) {
                model->first_byte -= BYTES_PER_PAGE;
            } else if(event->key == InputKeyRight && model->first_byte + BYTES_PER_PAGE < frame.length) {
                model->first_byte += BYTES_PER_PAGE;
            }
        }
        save = confirm && !model->saved;
    }
    view_commit_model(page->view, handled);
    if(state == TraceCaptureError && confirm) {
        start_job(page, TraceCapture);
        return true;
    }
    if(save) start_job(page, TraceSave);
    return handled;
}

static void trace_exit(void* context) {
    Hf14aTracePage* page = context;
    hf14a_trace_page_wait(page);
    TraceModel* model = view_get_model(page->view);
    model->state = TraceOpenError;
    model->data = NULL;
    free(page->data);
    page->data = NULL;
    view_commit_model(page->view, false);
}

Hf14aTracePage* hf14a_trace_page_create(void) {
    Hf14aTracePage* page = calloc(1, sizeof(Hf14aTracePage));
    page->view = view_alloc();
    page->path = furi_string_alloc();
    view_set_context(page->view, page);
    view_set_draw_callback(page->view, trace_draw);
    view_set_input_callback(page->view, trace_input);
    view_set_exit_callback(page->view, trace_exit);
    view_allocate_model(page->view, ViewModelTypeLocking, sizeof(TraceModel));
    return page;
}

void hf14a_trace_page_wait(Hf14aTracePage* page) {
    if(!page->worker) return;
    furi_thread_join(page->worker);
    furi_thread_free(page->worker);
    page->worker = NULL;
}

void hf14a_trace_page_free(Hf14aTracePage* page) {
    hf14a_trace_page_wait(page);
    free(page->data);
    furi_string_free(page->path);
    view_free(page->view);
    free(page);
}

void hf14a_trace_page_start(Hf14aTracePage* page, uint32_t previous_view) {
    page->previous_view = previous_view;
    start_job(page, TraceCapture);
}

bool hf14a_trace_page_browse(Hf14aTracePage* page, uint32_t previous_view) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FuriString* dir = trace_directory(storage);
    furi_record_close(RECORD_STORAGE);

    DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
    bool selected = false;
    if(dir) {
        DialogsFileBrowserOptions options;
        dialog_file_browser_set_basic_options(&options, TRACE_EXT, NULL);
        options.base_path = furi_string_get_cstr(dir);
        selected = dialog_file_browser_show(dialogs, page->path, dir, &options);
        furi_string_free(dir);
    } else {
        dialog_message_show_storage_error(dialogs, "Cannot open traces");
    }
    furi_record_close(RECORD_DIALOGS);
    if(selected) {
        page->previous_view = previous_view;
        start_job(page, TraceOpen);
    }
    return selected;
}

View* hf14a_trace_page_view(Hf14aTracePage* page) {
    return page->view;
}

uint32_t hf14a_trace_page_previous(void* context) {
    return ((Hf14aTracePage*)context)->previous_view;
}
