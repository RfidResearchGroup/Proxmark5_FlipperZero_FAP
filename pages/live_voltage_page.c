#include <furi.h>
#include <gui/elements.h>
#include <stdio.h>
#include <string.h>

#include "live_voltage_page.h"
#include "proxmark5_frame.h"

#define LIVE_VOLTAGE_HISTORY 96
#define LIVE_VOLTAGE_INTERVAL_MS 100
#define TUNE_CONTROL_TIMEOUT_MS 850
#define TUNE_MEASURE_TIMEOUT_MS 350
#define LIVE_VOLTAGE_STOP_FLAG (1U << 0)

typedef enum {
    TuneStart = 1,
    TuneMeasure = 2,
    TuneStop = 3,
} TuneAction;

typedef struct {
    const char* label;
    uint16_t command;
    uint8_t divisor;
} TuneBand;

// LF carrier is 12 MHz / (divisor + 1).
static const TuneBand tune_bands[] = {
    {"LF 125 kHz", CMD_MEASURE_ANTENNA_TUNING_LF, 95},
    {"LF 134.8 kHz", CMD_MEASURE_ANTENNA_TUNING_LF, 88},
    {"HF 13.56 MHz", CMD_MEASURE_ANTENNA_TUNING_HF, 0},
};

typedef enum {
    LiveVoltageIdle,
    LiveVoltageRunning,
    LiveVoltageStopping,
    LiveVoltageReadFailed,
    LiveVoltageStopFailed,
} LiveVoltageState;

typedef struct {
    LiveVoltageState state;
    bool return_when_stopped;
    uint8_t band;
    uint8_t next;
    uint8_t count;
    uint16_t history[LIVE_VOLTAGE_HISTORY];
} LiveVoltageModel;

struct LiveVoltagePage {
    View* view;
    FuriThread* worker;
    FuriEventFlag* stop;
    void (*return_to_antenna)(void*);
    void* callback_context;
    bool stop_only;
    volatile bool cancel;
};

static bool live_voltage_stop_requested(const LiveVoltagePage* page) {
    return (furi_event_flag_get(page->stop) & LIVE_VOLTAGE_STOP_FLAG) != 0;
}

static void live_voltage_request_stop(LiveVoltagePage* page) {
    page->cancel = true;
    furi_event_flag_set(page->stop, LIVE_VOLTAGE_STOP_FLAG);
}

static bool tune_retry(LiveVoltagePage* page, TuneAction action, int status) {
    if(status != PM3_ETIMEOUT && status != PM3_EIO) return false;
    bool stopping = action == TuneStop;
    return (stopping || !live_voltage_stop_requested(page)) && proxmark5_frame_synchronize() &&
           (stopping || !live_voltage_stop_requested(page));
}

static int tune_exchange(
    const TuneBand* band, TuneAction action, PacketResponseNG* reply, const volatile bool* cancel) {
    uint8_t payload[] = {action, band->divisor};
    size_t length = band->command == CMD_MEASURE_ANTENNA_TUNING_HF ? 1 : sizeof(payload);
    clearCommandBuffer();
    if(!SendCommandNG(band->command, payload, length)) return PM3_EIO;
    uint32_t timeout = action == TuneMeasure ? TUNE_MEASURE_TIMEOUT_MS : TUNE_CONTROL_TIMEOUT_MS;
    int status = proxmark5_frame_wait(band->command, reply, timeout, cancel);
    return status == PM3_SUCCESS ? reply->status : status;
}

static int tune_set_field(
    LiveVoltagePage* page,
    const TuneBand* band,
    TuneAction action,
    PacketResponseNG* reply) {
    const volatile bool* cancel = action == TuneStop ? NULL : &page->cancel;
    int status = tune_exchange(band, action, reply, cancel);
    if(tune_retry(page, action, status)) status = tune_exchange(band, action, reply, cancel);
    if(status == PM3_SUCCESS && reply->length != 0) return PM3_ESOFT;
    return status;
}

static int tune_read(
    LiveVoltagePage* page, const TuneBand* band, PacketResponseNG* reply, uint16_t* voltage_cv) {
    int status = tune_exchange(band, TuneMeasure, reply, &page->cancel);
    if(status != PM3_SUCCESS) return status;

    uint32_t millivolts;
    if(reply->length == sizeof(uint32_t)) {
        memcpy(&millivolts, reply->data.asBytes, sizeof(millivolts));
    } else if(band->command == CMD_MEASURE_ANTENNA_TUNING_HF &&
              reply->length == sizeof(uint16_t)) {
        uint16_t value;
        memcpy(&value, reply->data.asBytes, sizeof(value));
        millivolts = value;
    } else {
        return PM3_ESOFT;
    }

    uint32_t centivolts = millivolts / 10 + (millivolts % 10 >= 5);
    if(centivolts > UINT16_MAX) return PM3_ESOFT;
    *voltage_cv = centivolts;
    return PM3_SUCCESS;
}

static void live_voltage_add_sample(LiveVoltagePage* page, uint16_t voltage) {
    LiveVoltageModel* model = view_get_model(page->view);
    model->history[model->next] = voltage;
    model->next = (model->next + 1) % LIVE_VOLTAGE_HISTORY;
    if(model->count < LIVE_VOLTAGE_HISTORY) model->count++;
    view_commit_model(page->view, true);
}

static int32_t live_voltage_worker(void* context) {
    LiveVoltagePage* page = context;
    LiveVoltageModel* model = view_get_model(page->view);
    const TuneBand* band = &tune_bands[model->band];
    view_commit_model(page->view, false);

    PacketResponseNG reply;
    int status = PM3_SUCCESS;
    bool field_off = true;

    // A retry must still turn off a field started by a previous worker.
    if(proxmark5_frame_begin_operation(page->stop_only ? NULL : &page->cancel)) {
        if(!page->stop_only) {
            status = tune_set_field(page, band, TuneStart, &reply);
            while(status == PM3_SUCCESS && !live_voltage_stop_requested(page)) {
                uint16_t voltage;
                status = tune_read(page, band, &reply, &voltage);
                if(tune_retry(page, TuneMeasure, status)) {
                    status = tune_read(page, band, &reply, &voltage);
                }
                if(status != PM3_SUCCESS) break;
                live_voltage_add_sample(page, voltage);
                furi_event_flag_wait(
                    page->stop,
                    LIVE_VOLTAGE_STOP_FLAG,
                    FuriFlagWaitAny | FuriFlagNoClear,
                    furi_ms_to_ticks(LIVE_VOLTAGE_INTERVAL_MS));
            }
        }
        proxmark5_frame_set_operation_cancel(NULL);
        // Start, Measure and Stop share a reply ID.
        if(page->stop_only || status != PM3_SUCCESS) (void)proxmark5_frame_synchronize();
        field_off = tune_set_field(page, band, TuneStop, &reply) == PM3_SUCCESS;
        proxmark5_frame_end_operation();
    }

    model = view_get_model(page->view);
    if(!field_off) {
        model->state = LiveVoltageStopFailed;
    } else if(status != PM3_SUCCESS && status != PM3_EOPABORTED &&
              model->state != LiveVoltageStopping) {
        model->state = LiveVoltageReadFailed;
    } else {
        model->state = LiveVoltageIdle;
    }
    bool return_to_antenna = field_off && model->return_when_stopped;
    if(field_off) model->return_when_stopped = false;
    view_commit_model(page->view, true);
    if(return_to_antenna) page->return_to_antenna(page->callback_context);
    return 0;
}

static uint16_t live_voltage_sample(const LiveVoltageModel* model, unsigned index) {
    unsigned first = model->next + LIVE_VOLTAGE_HISTORY - model->count;
    return model->history[(first + index) % LIVE_VOLTAGE_HISTORY];
}

static void live_voltage_draw_history(Canvas* canvas, const LiveVoltageModel* model) {
    enum { Left = 28, Right = 124, Top = 28, Bottom = 51 };
    uint32_t low = live_voltage_sample(model, 0);
    uint32_t high = low;
    for(unsigned i = 1; i < model->count; ++i) {
        uint16_t sample = live_voltage_sample(model, i);
        low = MIN(low, sample);
        high = MAX(high, sample);
    }
    low = low / 100 * 100;
    high = MAX(low + 100, (high + 99) / 100 * 100);

    char label[16];
    snprintf(label, sizeof(label), "%lu V", (unsigned long)(high / 100));
    canvas_draw_str_aligned(canvas, Left - 3, Top + 3, AlignRight, AlignBottom, label);
    snprintf(label, sizeof(label), "%lu V", (unsigned long)(low / 100));
    canvas_draw_str_aligned(canvas, Left - 3, Bottom + 2, AlignRight, AlignBottom, label);
    canvas_draw_line(canvas, Left, Top, Left, Bottom);
    canvas_draw_line(canvas, Left, Bottom, Right, Bottom);

    uint8_t x = Right + 1 - model->count;
    uint8_t previous_y = Bottom;
    for(unsigned i = 0; i < model->count; ++i, ++x) {
        uint8_t y = Bottom - (live_voltage_sample(model, i) - low) * (Bottom - Top) / (high - low);
        if(i) {
            canvas_draw_line(canvas, x - 1, previous_y, x, y);
        } else {
            canvas_draw_dot(canvas, x, y);
        }
        previous_y = y;
    }
}

static void live_voltage_draw(Canvas* canvas, void* context) {
    const LiveVoltageModel* model = context;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 3, 10, "Live Voltage");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 3, 23, tune_bands[model->band].label);

    if(model->state == LiveVoltageStopFailed) {
        canvas_draw_str(canvas, 3, 40, "Field off unconfirmed");
        elements_button_center(canvas, "Retry Stop");
        return;
    }
    if(model->state == LiveVoltageStopping) {
        canvas_draw_str(canvas, 3, 40, "Stopping...");
        return;
    }

    if(model->state == LiveVoltageReadFailed) {
        canvas_draw_str(canvas, 3, 40, "Measurement failed");
    } else if(model->count) {
        uint32_t cv = live_voltage_sample(model, model->count - 1);
        char value[16];
        snprintf(value, sizeof(value), "%lu.%02lu V", (unsigned long)(cv / 100),
                 (unsigned long)(cv % 100));
        canvas_draw_str_aligned(canvas, 125, 23, AlignRight, AlignBottom, value);
        live_voltage_draw_history(canvas, model);
        if(model->state == LiveVoltageIdle) {
            canvas_draw_str_aligned(canvas, 125, 10, AlignRight, AlignBottom, "Held");
        }
    } else if(model->state == LiveVoltageRunning) {
        canvas_draw_str(canvas, 3, 40, "Starting...");
    }

    if(model->state == LiveVoltageRunning) {
        elements_button_center(canvas, "Stop");
    } else {
        elements_button_center(canvas, "Start");
        elements_button_right(canvas, "Band");
    }
}

void live_voltage_page_tick(LiveVoltagePage* page) {
    if(!page->worker || furi_thread_get_state(page->worker) != FuriThreadStateStopped) return;
    furi_thread_free(page->worker);
    page->worker = NULL;
}

void live_voltage_page_stop_worker(LiveVoltagePage* page) {
    if(!page->worker) return;
    live_voltage_request_stop(page);
    furi_thread_join(page->worker);
    furi_thread_free(page->worker);
    page->worker = NULL;
}

static bool live_voltage_input(InputEvent* event, void* context) {
    if(event->type != InputTypeShort) return false;

    LiveVoltagePage* page = context;
    LiveVoltageModel* model = view_get_model(page->view);
    if(model->state == LiveVoltageRunning &&
       (event->key == InputKeyOk || event->key == InputKeyBack)) {
        model->state = LiveVoltageStopping;
        model->return_when_stopped = event->key == InputKeyBack;
        live_voltage_request_stop(page);
        view_commit_model(page->view, true);
        return true;
    }
    if(model->state == LiveVoltageStopping) {
        if(event->key == InputKeyBack) model->return_when_stopped = true;
        view_commit_model(page->view, false);
        return true;
    }
    if(event->key == InputKeyBack && model->state == LiveVoltageStopFailed) {
        model->return_when_stopped = true;
        view_commit_model(page->view, false);
        return true;
    }
    if(event->key == InputKeyBack) {
        model->return_when_stopped = false;
        view_commit_model(page->view, false);
        return false;
    }
    if(event->key == InputKeyRight &&
       (model->state == LiveVoltageIdle || model->state == LiveVoltageReadFailed)) {
        model->band = (model->band + 1) % COUNT_OF(tune_bands);
        model->next = 0;
        model->count = 0;
        model->state = LiveVoltageIdle;
        view_commit_model(page->view, true);
        return true;
    }

    const bool start = event->key == InputKeyOk &&
                       (!page->worker ||
                        furi_thread_get_state(page->worker) == FuriThreadStateStopped);
    const bool stop_only = model->state == LiveVoltageStopFailed;
    if(start) {
        model->state = stop_only ? LiveVoltageStopping : LiveVoltageRunning;
        model->next = 0;
        model->count = 0;
    }
    view_commit_model(page->view, start);
    if(!start) return false;

    if(page->worker) furi_thread_free(page->worker);
    furi_event_flag_clear(page->stop, LIVE_VOLTAGE_STOP_FLAG);
    page->cancel = false;
    page->stop_only = stop_only;
    page->worker = furi_thread_alloc_ex("LiveVoltage", 4096, live_voltage_worker, page);
    furi_thread_start(page->worker);
    return true;
}

static void live_voltage_exit(void* context) {
    LiveVoltagePage* page = context;
    LiveVoltageModel* model = view_get_model(page->view);
    if(model->state == LiveVoltageRunning) {
        model->state = LiveVoltageStopping;
        live_voltage_request_stop(page);
    }
    view_commit_model(page->view, false);
}

LiveVoltagePage* live_voltage_page_create(void (*return_to_antenna)(void*), void* context) {
    LiveVoltagePage* page = calloc(1, sizeof(LiveVoltagePage));
    page->return_to_antenna = return_to_antenna;
    page->callback_context = context;
    page->stop = furi_event_flag_alloc();
    page->view = view_alloc();
    view_set_context(page->view, page);
    view_allocate_model(page->view, ViewModelTypeLocking, sizeof(LiveVoltageModel));
    LiveVoltageModel* model = view_get_model(page->view);
    *model = (LiveVoltageModel){.state = LiveVoltageIdle};
    view_commit_model(page->view, false);
    view_set_draw_callback(page->view, live_voltage_draw);
    view_set_input_callback(page->view, live_voltage_input);
    view_set_exit_callback(page->view, live_voltage_exit);
    return page;
}

void live_voltage_page_free(LiveVoltagePage* page) {
    live_voltage_page_stop_worker(page);
    furi_event_flag_free(page->stop);
    view_free_model(page->view);
    view_free(page->view);
    free(page);
}

View* live_voltage_page_get_view(LiveVoltagePage* page) {
    return page->view;
}
