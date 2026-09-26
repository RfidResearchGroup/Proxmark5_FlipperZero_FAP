#include <furi.h>
#include <gui/canvas.h>
#include <gui/elements.h>
#include <string.h>
#include "status_page.h"
#include "pm3_cmd.h"

typedef enum {
    StatusPageStateIdle,
    StatusPageStateRunning,
    StatusPageStateDone,
} StatusPageUiState;

typedef struct {
    StatusPageUiState ui_state;
    int result;
    char title[24];
    char lines[STATUS_PAGE_MAX_LINES][STATUS_PAGE_LINE_LEN];
    int line_count;
    int scroll_offset;
} StatusPageModel;

struct StatusPage {
    View* view;

    FuriThread* worker_thread;
    volatile bool worker_thread_running;
    volatile bool worker_thread_cancel_requested;

    StatusPageFetchFn fetch;

    StatusPageBackCallback back_callback;
    void* back_callback_context;
};

static void status_page_draw_callback(Canvas* canvas, void* context) {
    StatusPageModel* model = context;
    if(!model) {
        return;
    }

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 4, 10, model->title);
    canvas_set_font(canvas, FontSecondary);

    if(model->ui_state == StatusPageStateRunning) {
        canvas_draw_str(canvas, 4, 24, "Testing... please wait");
    } else if(model->ui_state == StatusPageStateIdle) {
        canvas_draw_str(canvas, 4, 24, "Ready");
    } else if(model->line_count == 0) {
        const char* msg = "Failed";
        if(model->result == PM3_ETIMEOUT) {
            msg = "Timeout";
        } else if(model->result == PM3_EOPABORTED) {
            msg = "Cancelled";
        }
        canvas_draw_str(canvas, 4, 24, msg);
    } else {
        int start = model->scroll_offset;
        int end = start + STATUS_PAGE_VISIBLE_LINES;
        if(end > model->line_count) {
            end = model->line_count;
        }
        int y = 22;
        for(int i = start; i < end; i++) {
            canvas_draw_str(canvas, 4, y, model->lines[i]);
            y += 11;
        }
        if(start > 0) {
            canvas_draw_str(canvas, 122, 20, "^");
        }
        if(end < model->line_count) {
            canvas_draw_str(canvas, 122, 46, "v");
        }
    }

    elements_button_left(canvas, "Back");
}

static bool status_page_input_callback(InputEvent* event, void* context) {
    StatusPage* status_page = context;
    if(!status_page || !event) {
        return false;
    }

    if(event->type == InputTypeShort && event->key == InputKeyLeft) {
        status_page_stop(status_page);
        if(status_page->back_callback) {
            status_page->back_callback(status_page->back_callback_context);
        }
        return true;
    }

    if((event->type == InputTypeShort || event->type == InputTypeRepeat) &&
       (event->key == InputKeyUp || event->key == InputKeyDown)) {
        StatusPageModel* model = view_get_model(status_page->view);
        bool changed = false;
        if(event->key == InputKeyUp && model->scroll_offset > 0) {
            model->scroll_offset--;
            changed = true;
        } else if(event->key == InputKeyDown &&
                  model->scroll_offset < model->line_count - STATUS_PAGE_VISIBLE_LINES) {
            model->scroll_offset++;
            changed = true;
        }
        view_commit_model(status_page->view, changed);
        return changed;
    }

    return false;
}

static void status_page_cleanup_worker(StatusPage* status_page) {
    if(!status_page || !status_page->worker_thread) {
        return;
    }
    if(status_page->worker_thread_running) {
        return;
    }
    furi_thread_join(status_page->worker_thread);
    furi_thread_free(status_page->worker_thread);
    status_page->worker_thread = NULL;
}

static int32_t status_page_worker(void* context) {
    StatusPage* status_page = context;
    if(!status_page) {
        return 0;
    }

    char lines[STATUS_PAGE_MAX_LINES][STATUS_PAGE_LINE_LEN];
    int count = 0;
    int result = status_page->fetch ?
                     status_page->fetch(&status_page->worker_thread_cancel_requested, lines, &count) :
                     PM3_ESOFT;

    if(count > STATUS_PAGE_MAX_LINES) {
        count = STATUS_PAGE_MAX_LINES;
    }
    if(count < 0) {
        count = 0;
    }

    StatusPageModel* model = view_get_model(status_page->view);
    model->result = result;
    model->ui_state = (result == PM3_EOPABORTED) ? StatusPageStateIdle : StatusPageStateDone;
    model->line_count = count;
    model->scroll_offset = 0;
    for(int i = 0; i < count; i++) {
        // memcpy, not snprintf("%s", ...): both buffers are the same fixed
        // size (already null-terminated within it by the fetch function), so
        // this can't truncate - avoids a -Wformat-truncation false positive
        // that gcc can't resolve between two identically-sized char arrays.
        memcpy(model->lines[i], lines[i], STATUS_PAGE_LINE_LEN);
    }
    view_commit_model(status_page->view, true);

    status_page->worker_thread_running = false;
    return 0;
}

StatusPage* status_page_create(StatusPageBackCallback back_callback, void* back_callback_context) {
    StatusPage* status_page = calloc(1, sizeof(StatusPage));
    if(!status_page) {
        return NULL;
    }

    status_page->view = view_alloc();
    if(!status_page->view) {
        free(status_page);
        return NULL;
    }

    status_page->back_callback = back_callback;
    status_page->back_callback_context = back_callback_context;

    view_set_context(status_page->view, status_page);
    // Locking: the worker thread mutates this model while the GUI thread draws.
    view_allocate_model(status_page->view, ViewModelTypeLocking, sizeof(StatusPageModel));
    view_set_draw_callback(status_page->view, status_page_draw_callback);
    view_set_input_callback(status_page->view, status_page_input_callback);

    StatusPageModel* model = view_get_model(status_page->view);
    model->ui_state = StatusPageStateIdle;
    model->result = PM3_SUCCESS;
    model->title[0] = '\0';
    model->line_count = 0;
    model->scroll_offset = 0;
    view_commit_model(status_page->view, false);

    return status_page;
}

void status_page_free(StatusPage* status_page) {
    if(!status_page) {
        return;
    }

    // Runs on the app thread after the view dispatcher has stopped, so
    // blocking here is safe - see status_page_stop()'s comment for why that
    // one must never block.
    status_page->worker_thread_cancel_requested = true;

    if(status_page->worker_thread) {
        furi_thread_join(status_page->worker_thread);
        furi_thread_free(status_page->worker_thread);
        status_page->worker_thread = NULL;
        status_page->worker_thread_running = false;
    }

    if(status_page->view) {
        view_set_context(status_page->view, NULL);
        view_free_model(status_page->view);
        view_free(status_page->view);
        status_page->view = NULL;
    }

    free(status_page);
}

View* status_page_get_view(StatusPage* status_page) {
    if(!status_page) {
        return NULL;
    }
    return status_page->view;
}

void status_page_start(StatusPage* status_page, const char* title, StatusPageFetchFn fetch) {
    if(!status_page || !status_page->view) {
        return;
    }

    status_page_cleanup_worker(status_page);
    if(status_page->worker_thread_running) {
        return;
    }

    status_page->fetch = fetch;

    StatusPageModel* model = view_get_model(status_page->view);
    snprintf(model->title, sizeof(model->title), "%s", title ? title : "");
    model->ui_state = StatusPageStateRunning;
    model->result = PM3_SUCCESS;
    model->line_count = 0;
    model->scroll_offset = 0;
    view_commit_model(status_page->view, true);

    status_page->worker_thread_cancel_requested = false;

    // Same stack sizing rationale as ReadHitag2Task: PacketResponseNG is
    // ~560 bytes and several live on this call path at once.
    status_page->worker_thread =
        furi_thread_alloc_ex("StatusPageTask", 8192, status_page_worker, status_page);
    if(!status_page->worker_thread) {
        model = view_get_model(status_page->view);
        model->ui_state = StatusPageStateDone;
        model->result = PM3_EMALLOC;
        view_commit_model(status_page->view, true);
        return;
    }

    status_page->worker_thread_running = true;
    furi_thread_start(status_page->worker_thread);
}

// See status_page.h - non-blocking on purpose, mirrors read_hitag2_page_stop().
void status_page_stop(StatusPage* status_page) {
    if(!status_page || !status_page->worker_thread) {
        return;
    }
    status_page->worker_thread_cancel_requested = true;
}
