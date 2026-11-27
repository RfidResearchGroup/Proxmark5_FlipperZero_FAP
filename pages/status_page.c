#include <furi.h>
#include <gui/canvas.h>
#include <gui/elements.h>
#include <string.h>
#include "status_page.h"
#include "status_page_tests.h"
#include "pm3_cmd.h"
#include "proxmark5_frame.h"

typedef enum {
    StatusPageStateRunning,
    StatusPageStateReady,
    StatusPageStateUnreachable,
} StatusPageUiState;

typedef enum {
    StatusTabCore,
    StatusTabPower,
    StatusTabBattery,
    StatusTabWiFi,
    StatusTabBle,
    StatusTabLink,
    StatusTabScheduler,
    STATUS_PAGE_PAGE_COUNT,
} StatusTab;

typedef struct {
    StatusPageUiState ui_state;
    int current_page;
    char rows[STATUS_PAGE_PAGE_COUNT][STATUS_PAGE_ROWS_PER_PAGE][STATUS_PAGE_LINE_LEN];
} StatusPageModel;

struct StatusPage {
    View* view;

    FuriThread* worker_thread;
    // The view model lock protects the worker state and pending restart.
    bool worker_thread_running;
    bool worker_restart_requested;
    volatile bool worker_thread_cancel_requested;
};

static const char* const status_page_titles[STATUS_PAGE_PAGE_COUNT] = {
    "Core",
    "Power",
    "Battery Capacity",
    "BWM Wi-Fi",
    "BWM BLE",
    "Link",
    "Scheduler"};

// Truncates with "..." using real measured glyph widths (FontSecondary is
// proportional, not monospace) rather than a guessed character budget.
static void status_page_draw_row(Canvas* canvas, int x, int y, const char* str) {
    if(!str || str[0] == '\0') {
        return;
    }
    int max_width = 124 - x;
    if(canvas_string_width(canvas, str) <= max_width) {
        canvas_draw_str(canvas, x, y, str);
        return;
    }
    char buf[STATUS_PAGE_LINE_LEN];
    size_t len = strlen(str);
    if(len >= sizeof(buf)) {
        len = sizeof(buf) - 1;
    }
    for(size_t cut = len; cut > 0; cut--) {
        snprintf(buf, sizeof(buf), "%.*s...", (int)cut, str);
        if(canvas_string_width(canvas, buf) <= max_width) {
            canvas_draw_str(canvas, x, y, buf);
            return;
        }
    }
    canvas_draw_str(canvas, x, y, "...");
}

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
    if(model->ui_state == StatusPageStateReady &&
       (model->current_page == StatusTabWiFi || model->current_page == StatusTabBle)) {
        canvas_draw_str_aligned(canvas, 104, 10, AlignRight, AlignBottom, "Snapshot");
    }
    if(model->ui_state == StatusPageStateRunning) {
        canvas_draw_str(canvas, 4, 24, "Reading status...");
    } else if(model->ui_state == StatusPageStateUnreachable) {
        canvas_draw_str(canvas, 4, 24, "Device not responding");
    } else {
        int y = 22;
        for(int i = 0; i < STATUS_PAGE_ROWS_PER_PAGE; i++) {
            status_page_draw_row(canvas, 4, y, model->rows[model->current_page][i]);
            y += 9;
        }
    }

    if(model->ui_state != StatusPageStateRunning) {
        elements_button_center(canvas, "Refresh");
    }
    if(model->ui_state == StatusPageStateReady) {
        elements_button_left(canvas, "Prev");
        elements_button_right(canvas, "Next");
    }
}

// InputKeyBack (the dedicated hardware button, not Left) is what actually
// exits this page - handled automatically via status_page_previous_callback,
// which already calls status_page_stop(). Left/Right only page here.
static bool status_page_input_callback(InputEvent* event, void* context) {
    StatusPage* status_page = context;
    if(!status_page || !event) {
        return false;
    }

    if(event->type == InputTypeShort && event->key == InputKeyOk) {
        status_page_start(status_page);
        return true;
    }

    if(event->type == InputTypeShort &&
       (event->key == InputKeyLeft || event->key == InputKeyRight)) {
        StatusPageModel* model = view_get_model(status_page->view);
        bool changed = model->ui_state == StatusPageStateReady;
        if(changed) {
            int step = event->key == InputKeyRight ? 1 : (STATUS_PAGE_PAGE_COUNT - 1);
            model->current_page = (model->current_page + step) % STATUS_PAGE_PAGE_COUNT;
        }
        view_commit_model(status_page->view, changed);
        return changed;
    }

    return false;
}

static void status_page_publish_tab(
    StatusPage* status_page,
    StatusTab tab,
    const char rows[STATUS_PAGE_ROWS_PER_PAGE][STATUS_PAGE_LINE_LEN]) {
    StatusPageModel* model = view_get_model(status_page->view);
    bool active = !status_page->worker_thread_cancel_requested;
    if(active) {
        memcpy(model->rows[tab], rows, sizeof(model->rows[tab]));
    }
    view_commit_model(status_page->view, active);
}

static void status_page_fill_battery_tabs(
    const bwm_battery_info_t* battery,
    char rows[STATUS_PAGE_PAGE_COUNT][STATUS_PAGE_ROWS_PER_PAGE][STATUS_PAGE_LINE_LEN]) {
    char lines[STATUS_PAGE_MAX_LINES][STATUS_PAGE_LINE_LEN];
    int count = status_test_format_battery(battery, lines);
    if(count == 8) {
        memcpy(rows[StatusTabPower], lines, sizeof(rows[0]));
        memcpy(rows[StatusTabBattery], lines + 4, sizeof(rows[0]));
    } else if(count == 1) {
        memcpy(rows[StatusTabPower][0], lines[0], STATUS_PAGE_LINE_LEN);
        memcpy(rows[StatusTabBattery][0], lines[0], STATUS_PAGE_LINE_LEN);
    }
}

static void status_page_scan(StatusPage* status_page) {
    volatile bool* cancel = &status_page->worker_thread_cancel_requested;
    char lines[STATUS_PAGE_MAX_LINES][STATUS_PAGE_LINE_LEN];
    char rows[STATUS_PAGE_PAGE_COUNT][STATUS_PAGE_ROWS_PER_PAGE][STATUS_PAGE_LINE_LEN];
    memset(rows, 0, sizeof(rows));
    int count = 0;

    // Measure the initial status scan before the separate radio requests.
    FuriThreadList* thread_list = furi_thread_list_alloc();
    furi_thread_enumerate(thread_list);
    uint32_t stats_start_tick = furi_get_tick();

    bool bwm_compiled = false;
    int caps_result = status_test_capabilities_fetch(cancel, lines, &count, &bwm_compiled);
    if(*cancel) {
        furi_thread_list_free(thread_list);
        return;
    }

    // Skip the remaining requests if the device does not answer.
    if(caps_result == PM3_ETIMEOUT || caps_result == PM3_EIO) {
        furi_thread_list_free(thread_list);
        StatusPageModel* model = view_get_model(status_page->view);
        bool active = !*cancel;
        if(active) {
            model->ui_state = StatusPageStateUnreachable;
        }
        view_commit_model(status_page->view, active);
        return;
    }

    if(count >= 6) {
        memcpy(rows[StatusTabCore][0], lines[1], STATUS_PAGE_LINE_LEN);
        memcpy(rows[StatusTabCore][2], lines[4], STATUS_PAGE_LINE_LEN);
        memcpy(rows[StatusTabCore][3], lines[5], STATUS_PAGE_LINE_LEN);
    } else {
        snprintf(rows[StatusTabCore][0], STATUS_PAGE_LINE_LEN, "BWM/CEP:   N/A");
        snprintf(rows[StatusTabCore][2], STATUS_PAGE_LINE_LEN, "MaxCmdData:   N/A");
        snprintf(rows[StatusTabCore][3], STATUS_PAGE_LINE_LEN, "Link:   N/A");
    }

    status_test_chip_id_fetch(cancel, rows[StatusTabCore][1]);
    if(*cancel) {
        furi_thread_list_free(thread_list);
        return;
    }

    bwm_battery_info_t battery;
    int cep_result = status_test_cep_fetch(cancel, rows[StatusTabLink], &battery);
    if(*cancel) {
        furi_thread_list_free(thread_list);
        return;
    }

    uint32_t rtt_ms;
    bool echo_received = status_test_ping_fetch(&rtt_ms);
    if(*cancel) {
        furi_thread_list_free(thread_list);
        return;
    }
    if(echo_received) {
        snprintf(rows[StatusTabLink][2], STATUS_PAGE_LINE_LEN, "RTT:   %lums", (unsigned long)rtt_ms);
        snprintf(rows[StatusTabLink][3], STATUS_PAGE_LINE_LEN, "Echo:   OK");
    } else {
        snprintf(rows[StatusTabLink][2], STATUS_PAGE_LINE_LEN, "RTT:   N/A");
        snprintf(rows[StatusTabLink][3], STATUS_PAGE_LINE_LEN, "Echo:   N/A");
    }

    if(bwm_compiled) {
        status_page_fill_battery_tabs(cep_result == PM3_SUCCESS ? &battery : NULL, rows);
        snprintf(rows[StatusTabWiFi][0], STATUS_PAGE_LINE_LEN, "Reading Wi-Fi...");
        snprintf(rows[StatusTabBle][0], STATUS_PAGE_LINE_LEN, "Reading BLE...");
    } else {
        const char* availability = caps_result == PM3_SUCCESS ? "BWM support unavailable" :
                                                               "Capabilities unavailable";
        snprintf(rows[StatusTabPower][0], STATUS_PAGE_LINE_LEN, "%s", availability);
        snprintf(rows[StatusTabBattery][0], STATUS_PAGE_LINE_LEN, "%s", availability);
        snprintf(rows[StatusTabWiFi][0], STATUS_PAGE_LINE_LEN, "%s", availability);
        snprintf(rows[StatusTabBle][0], STATUS_PAGE_LINE_LEN, "%s", availability);
    }

    furi_thread_enumerate(thread_list);
    uint32_t stats_window_ms = furi_get_tick() - stats_start_tick;
    float spi_rx_cpu = 0.0f;
    for(size_t i = 0; i < furi_thread_list_size(thread_list); i++) {
        FuriThreadListItem* item = furi_thread_list_get_at(thread_list, i);
        if(item->name && strcmp(item->name, "PM5_SPI_RX") == 0) {
            spi_rx_cpu = item->cpu;
            break;
        }
    }
    float isr_time = furi_thread_list_get_isr_time(thread_list);
    furi_thread_list_free(thread_list);

    snprintf(rows[StatusTabScheduler][0], STATUS_PAGE_LINE_LEN, "SPI_RX cpu:   %.1f%%", (double)spi_rx_cpu);
    snprintf(rows[StatusTabScheduler][1], STATUS_PAGE_LINE_LEN, "ISR:   %.2f%%", (double)isr_time);
    snprintf(rows[StatusTabScheduler][2], STATUS_PAGE_LINE_LEN, "Window:   %lums", (unsigned long)stats_window_ms);

    StatusPageModel* model = view_get_model(status_page->view);
    if(*cancel) {
        view_commit_model(status_page->view, false);
        return;
    }
    model->ui_state = StatusPageStateReady;
    memcpy(model->rows, rows, sizeof(rows));
    view_commit_model(status_page->view, true);

    if(bwm_compiled) {
        memset(rows[StatusTabWiFi], 0, sizeof(rows[0]));
        status_test_wifi_fetch(cancel, rows[StatusTabWiFi]);
        if(*cancel) {
            return;
        }
        status_page_publish_tab(status_page, StatusTabWiFi, rows[StatusTabWiFi]);

        memset(rows[StatusTabBle], 0, sizeof(rows[0]));
        status_test_ble_fetch(cancel, rows[StatusTabBle]);
        if(*cancel) {
            return;
        }
        status_page_publish_tab(status_page, StatusTabBle, rows[StatusTabBle]);
    }
}

static int32_t status_page_worker(void* context) {
    StatusPage* status_page = context;
    for(;;) {
        if(proxmark5_frame_begin_operation(&status_page->worker_thread_cancel_requested)) {
            status_page_scan(status_page);
            proxmark5_frame_end_operation();
        }

        view_get_model(status_page->view);
        if(!status_page->worker_restart_requested) {
            status_page->worker_thread_running = false;
            view_commit_model(status_page->view, false);
            return 0;
        }
        status_page->worker_restart_requested = false;
        status_page->worker_thread_cancel_requested = false;
        view_commit_model(status_page->view, false);
    }
}

StatusPage* status_page_create(void) {
    StatusPage* status_page = calloc(1, sizeof(StatusPage));
    if(!status_page) {
        return NULL;
    }

    status_page->view = view_alloc();
    if(!status_page->view) {
        free(status_page);
        return NULL;
    }

    view_set_context(status_page->view, status_page);
    // Locking: the worker thread mutates this model while the GUI thread draws.
    view_allocate_model(status_page->view, ViewModelTypeLocking, sizeof(StatusPageModel));
    view_set_draw_callback(status_page->view, status_page_draw_callback);
    view_set_input_callback(status_page->view, status_page_input_callback);

    StatusPageModel* model = view_get_model(status_page->view);
    model->ui_state = StatusPageStateRunning;
    model->current_page = StatusTabCore;
    memset(model->rows, 0, sizeof(model->rows));
    view_commit_model(status_page->view, false);

    return status_page;
}

void status_page_tick(StatusPage* page) {
    if(!page->worker_thread || furi_thread_get_state(page->worker_thread) != FuriThreadStateStopped) return;
    furi_thread_free(page->worker_thread);
    page->worker_thread = NULL;
}

void status_page_stop_worker(StatusPage* status_page) {
    if(!status_page || !status_page->worker_thread) return;
    status_page_stop(status_page);
    furi_thread_join(status_page->worker_thread);
    furi_thread_free(status_page->worker_thread);
    status_page->worker_thread = NULL;
}

void status_page_free(StatusPage* status_page) {
    if(!status_page) {
        return;
    }

    status_page_stop_worker(status_page);
    if(status_page->view) {
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

    StatusPageModel* model = view_get_model(status_page->view);
    bool running = status_page->worker_thread_running;
    if(running && !status_page->worker_thread_cancel_requested) {
        view_commit_model(status_page->view, false);
        return;
    }

    model->ui_state = StatusPageStateRunning;
    memset(model->rows, 0, sizeof(model->rows));
    status_page->worker_restart_requested = running;
    view_commit_model(status_page->view, true);
    if(running) {
        return;
    }

    if(status_page->worker_thread) {
        furi_thread_join(status_page->worker_thread);
        furi_thread_free(status_page->worker_thread);
    }

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

    view_get_model(status_page->view);
    status_page->worker_thread_cancel_requested = false;
    status_page->worker_thread_running = true;
    view_commit_model(status_page->view, false);
    furi_thread_start(status_page->worker_thread);
}

void status_page_stop(StatusPage* status_page) {
    if(!status_page) {
        return;
    }

    view_get_model(status_page->view);
    status_page->worker_thread_cancel_requested = true;
    status_page->worker_restart_requested = false;
    view_commit_model(status_page->view, false);
}
