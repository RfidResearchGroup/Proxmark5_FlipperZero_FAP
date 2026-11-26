#include <furi.h>
#include <gui/canvas.h>
#include <gui/elements.h>
#include "simple_reader_page.h"
#include "pm3_cmd.h"
#include "../readers/lf_capture.h"
#include "proxmark5_frame.h"

typedef struct {
    const SimpleReader* reader;
    bool running;
    bool saved;
    int status;
    SimpleReaderResult result;
} SimpleReaderModel;

struct SimpleReaderPage {
    View* view;
    FuriThread* worker;
    SimpleReaderDetailsCallback on_details;
    void* callback_context;
    // The view model lock protects the worker state.
    const SimpleReader* next_reader;
    volatile bool cancel;
};

static void simple_reader_draw(Canvas* canvas, void* context) {
    const SimpleReaderModel* model = context;
    canvas_clear(canvas);
    if(!model->reader) return;
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 4, 11, model->reader->title);
    canvas_set_font(canvas, FontSecondary);

    if(model->running) {
        canvas_draw_str(canvas, 4, 27, model->reader->prompt);
        canvas_draw_str(canvas, 4, 40, "Reading...");
        return;
    }

    char line[32];
    switch(model->status) {
    case PM3_SUCCESS: {
        size_t count = MIN(model->result.line_count, SIMPLE_READER_MAX_LINES);
        uint8_t y = count == 1 ? 30 : 25;
        for(size_t i = 0; i < count; i++, y += 12) {
            canvas_draw_str(canvas, 4, y, model->result.lines[i]);
        }
        break;
    }
    case PM3_ENODATA:
        canvas_draw_str(canvas, 4, 25, model->reader->no_data);
        canvas_draw_str(canvas, 4, 38, model->reader->no_data_hint);
        break;
    case LfCaptureRestoreFailed:
        canvas_draw_str(canvas, 4, 25, "LF settings may have");
        canvas_draw_str(canvas, 4, 38, "changed. Check on PM5.");
        break;
    case PM3_EMALLOC:
        canvas_draw_str(canvas, 4, 27, "Not enough memory");
        break;
    case PM3_EIO:
        canvas_draw_str(canvas, 4, 27, "Send failed. Check link.");
        break;
    case PM3_ETIMEOUT:
        canvas_draw_str(canvas, 4, 25, "No reply from PM5");
        canvas_draw_str(canvas, 4, 38, "Check PM5 connection");
        break;
    case PM3_ENOTIMPL:
        canvas_draw_str(canvas, 4, 27, "Reader unavailable");
        break;
    case PM3_EOPABORTED:
        canvas_draw_str(canvas, 4, 27, "Read cancelled");
        break;
    default:
        snprintf(line, sizeof(line), "Read error (%d)", model->status);
        canvas_draw_str(canvas, 4, 27, line);
        break;
    }
    if(!model->saved) elements_button_center(canvas, "Read again");
    if(model->status == PM3_SUCCESS) {
        if(model->reader->format_details) {
            elements_button_right(canvas, "Data");
            if(!model->saved && model->reader->save) elements_button_left(canvas, "Save");
        } else if(!model->saved && model->reader->save) {
            elements_button_right(canvas, "Save");
        }
    }
}

static int32_t simple_reader_worker(void* context) {
    SimpleReaderPage* page = context;
    for(;;) {
        SimpleReaderModel* model = view_get_model(page->view);
        const SimpleReader* reader = page->next_reader;
        page->next_reader = NULL;
        if(!reader) {
            view_commit_model(page->view, false);
            return 0;
        }
        page->cancel = false;
        view_commit_model(page->view, false);

        SimpleReaderResult result = {0};
        int status = PM3_EOPABORTED;
        if(proxmark5_frame_begin_operation(&page->cancel)) {
            result.data = lf_capture_alloc(reader->data_size);
            status = result.data ? reader->read(&result) : PM3_EMALLOC;
            proxmark5_frame_end_operation();
        }

        model = view_get_model(page->view);
        const bool publish = !page->cancel;
        if(publish) {
            model->running = false;
            model->status = status;
            model->result = result;
        } else {
            free(result.data);
        }
        view_commit_model(page->view, publish);
    }
}

void simple_reader_page_start(SimpleReaderPage* page, const SimpleReader* reader) {
    SimpleReaderModel* model = view_get_model(page->view);
    page->next_reader = reader;
    page->cancel = true;
    free(model->result.data);
    model->result = (SimpleReaderResult){0};
    model->reader = reader;
    model->running = true;
    model->saved = false;
    view_commit_model(page->view, true);
    simple_reader_page_tick(page);
}

void simple_reader_page_tick(SimpleReaderPage* page) {
    if(page->worker) {
        if(furi_thread_get_state(page->worker) != FuriThreadStateStopped) return;
        furi_thread_free(page->worker);
        page->worker = NULL;
    }
    view_get_model(page->view);
    const bool pending = page->next_reader != NULL;
    view_commit_model(page->view, false);
    if(pending) {
        page->worker = furi_thread_alloc_ex("PM5Reader", 4096, simple_reader_worker, page);
        furi_thread_start(page->worker);
    }
}

void simple_reader_page_stop(SimpleReaderPage* page) {
    SimpleReaderModel* model = view_get_model(page->view);
    page->next_reader = NULL;
    page->cancel = true;
    model->running = false;
    model->saved = false;
    free(model->result.data);
    model->result = (SimpleReaderResult){0};
    view_commit_model(page->view, false);
}

static bool simple_reader_input(InputEvent* event, void* context) {
    SimpleReaderPage* page = context;
    if(event->type != InputTypeShort) return false;
    if(event->key == InputKeyRight || event->key == InputKeyLeft) {
        SimpleReaderModel* model = view_get_model(page->view);
        const SimpleReader* reader = model->reader;
        bool saved = model->saved;
        const uint8_t* data = !model->running && model->status == PM3_SUCCESS ? model->result.data : NULL;
        view_commit_model(page->view, false);
        if(!data || !reader) return false;
        if(reader->format_details && event->key == InputKeyRight) {
            page->on_details(page->callback_context);
            return true;
        }
        if(!saved && reader->save && (event->key == InputKeyRight || reader->format_details)) {
            reader->save(data);
            return true;
        }
        return false;
    }
    if(event->key != InputKeyOk) return false;
    SimpleReaderModel* model = view_get_model(page->view);
    const SimpleReader* reader = (model->running || model->saved) ? NULL : model->reader;
    view_commit_model(page->view, false);
    if(reader) simple_reader_page_start(page, reader);
    return true;
}

bool simple_reader_page_show_saved(SimpleReaderPage* page, const SimpleReader* reader, const uint8_t* data) {
    uint8_t* copy = lf_capture_alloc(reader->data_size);
    if(!copy) return false;
    memcpy(copy, data, reader->data_size);

    simple_reader_page_stop(page);
    SimpleReaderModel* model = view_get_model(page->view);
    model->reader = reader;
    model->saved = true;
    model->status = PM3_SUCCESS;
    model->result.data = copy;
    reader->format(&model->result);
    view_commit_model(page->view, true);
    return true;
}

bool simple_reader_page_is_saved(SimpleReaderPage* page) {
    SimpleReaderModel* model = view_get_model(page->view);
    bool saved = model->saved;
    view_commit_model(page->view, false);
    return saved;
}

bool simple_reader_page_format_details(SimpleReaderPage* page, FuriString* text) {
    SimpleReaderModel* model = view_get_model(page->view);
    bool available = !model->running && model->reader && model->result.data &&
                     model->status == PM3_SUCCESS && model->reader->format_details;
    if(available) model->reader->format_details(model->result.data, text);
    view_commit_model(page->view, false);
    return available;
}

SimpleReaderPage* simple_reader_page_create(SimpleReaderDetailsCallback on_details, void* context) {
    SimpleReaderPage* page = calloc(1, sizeof(SimpleReaderPage));
    page->view = view_alloc();
    page->on_details = on_details;
    page->callback_context = context;
    view_set_context(page->view, page);
    view_set_draw_callback(page->view, simple_reader_draw);
    view_set_input_callback(page->view, simple_reader_input);
    view_allocate_model(page->view, ViewModelTypeLocking, sizeof(SimpleReaderModel));
    SimpleReaderModel* model = view_get_model(page->view);
    model->reader = NULL;
    view_commit_model(page->view, false);
    return page;
}

void simple_reader_page_stop_worker(SimpleReaderPage* page) {
    if(!page) return;
    simple_reader_page_stop(page);
    if(!page->worker) return;
    furi_thread_join(page->worker);
    furi_thread_free(page->worker);
    page->worker = NULL;
}

void simple_reader_page_free(SimpleReaderPage* page) {
    simple_reader_page_stop_worker(page);
    view_free(page->view);
    free(page);
}

View* simple_reader_page_get_view(SimpleReaderPage* page) {
    return page->view;
}
