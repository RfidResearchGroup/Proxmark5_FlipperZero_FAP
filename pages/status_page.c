#include <furi.h>
#include <gui/canvas.h>
#include <gui/elements.h>
#include <string.h>
#include "status_page.h"
#include "status_page_tests.h"
#include "pm3_cmd.h"

typedef enum {
    StatusPageStateRunning,
    StatusPageStateReady,
    StatusPageStateUnreachable,
} StatusPageUiState;

typedef struct {
    StatusPageUiState ui_state;
    int current_page;
    char rows[STATUS_PAGE_PAGE_COUNT][STATUS_PAGE_ROWS_PER_PAGE][STATUS_PAGE_LINE_LEN];
} StatusPageModel;

struct StatusPage {
    View* view;

    FuriThread* worker_thread;
    volatile bool worker_thread_running;
    volatile bool worker_thread_cancel_requested;

    StatusPageBackCallback back_callback;
    void* back_callback_context;
};

static const char* const status_page_titles[STATUS_PAGE_PAGE_COUNT] = {"Core", "Power", "Link"};

static void status_page_draw_callback(Canvas* canvas, void* context) {
    StatusPageModel* model = context;
    if(!model) {
        return;
    }

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 4, 10, status_page_titles[model->current_page]);

    // uint8_t, not int: lets the compiler bound %d's max width for
    // -Wformat-truncation instead of assuming a full int's worth of digits.
    char page_indicator[8];
    uint8_t page_num = (uint8_t)(model->current_page + 1);
    uint8_t page_total = STATUS_PAGE_PAGE_COUNT;
    snprintf(page_indicator, sizeof(page_indicator), "%u/%u", page_num, page_total);
    canvas_draw_str_aligned(canvas, 124, 10, AlignRight, AlignBottom, page_indicator);

    canvas_set_font(canvas, FontSecondary);
    if(model->ui_state == StatusPageStateRunning) {
        canvas_draw_str(canvas, 4, 24, "Testing... please wait");
    } else if(model->ui_state == StatusPageStateUnreachable) {
        canvas_draw_str(canvas, 4, 24, "Device not responding");
    } else {
        int y = 22;
        for(int i = 0; i < STATUS_PAGE_ROWS_PER_PAGE; i++) {
            if(model->rows[model->current_page][i][0] != '\0') {
                canvas_draw_str(canvas, 4, y, model->rows[model->current_page][i]);
            }
            y += 9;
        }
    }

    elements_button_left(canvas, "Back");
    if(model->ui_state == StatusPageStateReady) {
        elements_button_right(canvas, "Next");
    }
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

    if(event->type == InputTypeShort && event->key == InputKeyRight) {
        StatusPageModel* model = view_get_model(status_page->view);
        bool changed = model->ui_state == StatusPageStateReady;
        if(changed) {
            model->current_page = (model->current_page + 1) % STATUS_PAGE_PAGE_COUNT;
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

static bool status_page_worker_check_cancel(StatusPage* status_page) {
    if(!status_page->worker_thread_cancel_requested) {
        return false;
    }
    status_page->worker_thread_running = false;
    return true;
}

static int32_t status_page_worker(void* context) {
    StatusPage* status_page = context;
    if(!status_page) {
        return 0;
    }

    volatile bool* cancel = &status_page->worker_thread_cancel_requested;
    char lines[STATUS_PAGE_MAX_LINES][STATUS_PAGE_LINE_LEN];
    char rows[STATUS_PAGE_PAGE_COUNT][STATUS_PAGE_ROWS_PER_PAGE][STATUS_PAGE_LINE_LEN];
    memset(rows, 0, sizeof(rows));
    int count = 0;

    int caps_result = status_test_capabilities_fetch(cancel, lines, &count);
    if(status_page_worker_check_cancel(status_page)) {
        return 0;
    }

    // No reply at all means the device isn't responding - skip the other 4
    // tests (they'd all time out too) and show one error page instead.
    if(caps_result == PM3_ETIMEOUT) {
        StatusPageModel* model = view_get_model(status_page->view);
        model->ui_state = StatusPageStateUnreachable;
        view_commit_model(status_page->view, true);
        status_page->worker_thread_running = false;
        return 0;
    }

    if(count >= 6) {
        memcpy(rows[0][0], lines[1], STATUS_PAGE_LINE_LEN);
        memcpy(rows[0][2], lines[4], STATUS_PAGE_LINE_LEN);
        memcpy(rows[0][3], lines[5], STATUS_PAGE_LINE_LEN);
    } else {
        snprintf(rows[0][0], STATUS_PAGE_LINE_LEN, "BWM/CEP: N/A");
        snprintf(rows[0][2], STATUS_PAGE_LINE_LEN, "MaxCmdData: N/A");
        snprintf(rows[0][3], STATUS_PAGE_LINE_LEN, "Baud: N/A");
    }

    count = 0;
    status_test_flash_chip_fetch(cancel, lines, &count);
    if(status_page_worker_check_cancel(status_page)) {
        return 0;
    }
    // ChipID is always this fetch fn's last line, whatever its index.
    if(count > 0) {
        memcpy(rows[0][1], lines[count - 1], STATUS_PAGE_LINE_LEN);
    } else {
        snprintf(rows[0][1], STATUS_PAGE_LINE_LEN, "ChipID: N/A");
    }

    count = 0;
    status_test_battery_fetch(cancel, lines, &count);
    if(status_page_worker_check_cancel(status_page)) {
        return 0;
    }
    if(count >= 1) {
        memcpy(rows[1][0], lines[0], STATUS_PAGE_LINE_LEN);
    }
    if(count >= 2) {
        memcpy(rows[1][1], lines[1], STATUS_PAGE_LINE_LEN);
    }
    if(count >= 3) {
        memcpy(rows[1][2], lines[2], STATUS_PAGE_LINE_LEN);
    }
    {
        // Fault's index isn't fixed (Health may or may not precede it) -
        // find it by prefix. Shown in place of Temp when present, since a
        // real fault is more actionable than ambient temperature.
        const char* fault_line = NULL;
        for(int i = 4; i < count; i++) {
            if(strncmp(lines[i], "Fault:", 6) == 0) {
                fault_line = lines[i];
                break;
            }
        }
        if(fault_line) {
            memcpy(rows[1][3], fault_line, STATUS_PAGE_LINE_LEN);
        } else if(count >= 4) {
            memcpy(rows[1][3], lines[3], STATUS_PAGE_LINE_LEN);
        }
    }

    count = 0;
    status_test_ping_fetch(cancel, lines, &count);
    if(status_page_worker_check_cancel(status_page)) {
        return 0;
    }
    char ping_rtt[STATUS_PAGE_LINE_LEN];
    char ping_echo[STATUS_PAGE_LINE_LEN];
    if(count >= 3) {
        memcpy(ping_rtt, lines[2], STATUS_PAGE_LINE_LEN);
        memcpy(ping_echo, lines[1], STATUS_PAGE_LINE_LEN);
    } else {
        snprintf(ping_rtt, STATUS_PAGE_LINE_LEN, "RTT: N/A");
        snprintf(ping_echo, STATUS_PAGE_LINE_LEN, "Echo: N/A");
    }

    count = 0;
    status_test_cep_fetch(cancel, lines, &count);
    if(status_page_worker_check_cancel(status_page)) {
        return 0;
    }
    if(count >= 1) {
        memcpy(rows[2][0], lines[0], STATUS_PAGE_LINE_LEN);
    }
    if(count >= 3) {
        memcpy(rows[2][1], lines[2], STATUS_PAGE_LINE_LEN);
    }
    memcpy(rows[2][2], ping_rtt, STATUS_PAGE_LINE_LEN);
    memcpy(rows[2][3], ping_echo, STATUS_PAGE_LINE_LEN);

    StatusPageModel* model = view_get_model(status_page->view);
    model->ui_state = StatusPageStateReady;
    model->current_page = 0;
    memcpy(model->rows, rows, sizeof(rows));
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
    model->ui_state = StatusPageStateRunning;
    model->current_page = 0;
    memset(model->rows, 0, sizeof(model->rows));
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

void status_page_start(StatusPage* status_page) {
    if(!status_page || !status_page->view) {
        return;
    }

    status_page_cleanup_worker(status_page);

    // Check cleanup_worker()'s pointer, not worker_thread_running - that flag
    // can flip false between the two checks and leak the old FuriThread.
    if(status_page->worker_thread) {
        return;
    }

    StatusPageModel* model = view_get_model(status_page->view);
    model->ui_state = StatusPageStateRunning;
    model->current_page = 0;
    memset(model->rows, 0, sizeof(model->rows));
    view_commit_model(status_page->view, true);

    status_page->worker_thread_cancel_requested = false;

    // Same stack sizing rationale as ReadHitag2Task: PacketResponseNG is
    // ~560 bytes and several live on this call path at once.
    status_page->worker_thread =
        furi_thread_alloc_ex("StatusPageTask", 8192, status_page_worker, status_page);
    if(!status_page->worker_thread) {
        model = view_get_model(status_page->view);
        model->ui_state = StatusPageStateUnreachable;
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
