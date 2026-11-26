#include <furi.h>
#include <gui/elements.h>
#include <stdio.h>
#include <string.h>

#include "antenna_check_page.h"
#include "proxmark5_frame.h"

#define ANTENNA_CHECK_TIMEOUT_MS 10000
#define SWEEP_DIVISOR_MIN 19
#define SWEEP_DIVISOR_MAX 255
#define SWEEP_MV_PER_STEP 512U

// CMD_MEASURE_ANTENNA_TUNING reply from MeasureAntennaTuning().
typedef struct {
    uint32_t lf134_mv;
    uint32_t lf125_mv;
    uint32_t configured_lf_mv;
    uint32_t hf_mv;
    uint32_t peak_mv;
    uint32_t peak_divisor;
    int32_t configured_divisor;
    uint8_t sweep[256];
} AntennaCheckReply;

_Static_assert(sizeof(AntennaCheckReply) == 284, "Unexpected antenna reply size");

typedef enum {
    AntennaCheckIdle,
    AntennaCheckRunning,
    AntennaCheckReady,
    AntennaCheckTimeout,
    AntennaCheckError,
} AntennaCheckState;

typedef struct {
    AntennaCheckState state;
    bool show_sweep;
    uint32_t lf125_mv;
    uint32_t lf134_mv;
    uint32_t hf_mv;
    uint32_t peak_divisor;
    uint8_t sweep[256];
} AntennaCheckModel;

struct AntennaCheckPage {
    View* view;
    FuriThread* worker;
    volatile bool cancel;
    void (*open_live)(void*);
    void* callback_context;
};

static void draw_voltage(Canvas* canvas, uint8_t y, const char* label, uint32_t mv) {
    char value[24];
    uint32_t centivolts = mv / 10 + (mv % 10 >= 5);
    snprintf(value, sizeof(value), "%lu.%02lu V", (unsigned long)(centivolts / 100),
             (unsigned long)(centivolts % 100));
    canvas_draw_str(canvas, 3, y, label);
    canvas_draw_str_aligned(canvas, 125, y, AlignRight, AlignBottom, value);
}

static uint32_t lf_frequency_centikhz(uint32_t divisor) {
    return (1200000 + (divisor + 1) / 2) / (divisor + 1);
}

static void antenna_check_peak(char* label, size_t size, uint32_t divisor) {
    if(divisor < SWEEP_DIVISOR_MIN || divisor > SWEEP_DIVISOR_MAX) {
        snprintf(label, size, "N/A");
        return;
    }
    uint32_t freq = lf_frequency_centikhz(divisor);
    snprintf(label, size, "%lu.%02lu kHz", (unsigned long)(freq / 100),
             (unsigned long)(freq % 100));
}

static void antenna_check_draw_arrow(Canvas* canvas, bool up, const char* label) {
    uint8_t tip = up ? 56 : 62;
    uint8_t ends = up ? 60 : 58;
    canvas_draw_line(canvas, 2, ends, 6, tip);
    canvas_draw_line(canvas, 6, tip, 10, ends);
    canvas_draw_str(canvas, 14, 63, label);
}

static void antenna_check_draw_sweep(Canvas* canvas, const AntennaCheckModel* model) {
    enum { Left = 26, Right = 124, Top = 16, Bottom = 42 };
    const uint32_t low = lf_frequency_centikhz(SWEEP_DIVISOR_MAX);
    const uint32_t high = lf_frequency_centikhz(SWEEP_DIVISOR_MIN);
    uint8_t peak = 0;
    for(int divisor = SWEEP_DIVISOR_MAX; divisor >= SWEEP_DIVISOR_MIN; --divisor) {
        peak = MAX(peak, model->sweep[divisor]);
    }
    uint32_t top_v = MAX(1U, (peak * SWEEP_MV_PER_STEP + 999) / 1000);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 3, 10, "LF Sweep");
    canvas_set_font(canvas, FontSecondary);
    char value[24];
    antenna_check_peak(value, sizeof(value), model->peak_divisor);
    canvas_draw_str_aligned(canvas, 125, 10, AlignRight, AlignBottom, value);
    snprintf(value, sizeof(value), "%lu V", (unsigned long)top_v);
    canvas_draw_str_aligned(canvas, 23, Top + 3, AlignRight, AlignBottom, value);
    canvas_draw_str_aligned(canvas, 23, Bottom + 2, AlignRight, AlignBottom, "0");
    canvas_draw_line(canvas, Left, Top, Left, Bottom);
    canvas_draw_line(canvas, Left, Bottom, Right, Bottom);
    canvas_draw_str(canvas, Left, 51, "47");
    canvas_draw_str_aligned(canvas, Right, 51, AlignRight, AlignBottom, "600 kHz");

    uint8_t prev_x = Left;
    uint8_t prev_y = Bottom;
    for(int divisor = SWEEP_DIVISOR_MAX; divisor >= SWEEP_DIVISOR_MIN; --divisor) {
        uint32_t freq = lf_frequency_centikhz(divisor);
        uint8_t x = Left + (freq - low) * (Right - Left) / (high - low);
        uint32_t mv = model->sweep[divisor] * SWEEP_MV_PER_STEP;
        uint8_t y = Bottom - mv * (Bottom - Top) / (top_v * 1000);
        if(divisor != SWEEP_DIVISOR_MAX) canvas_draw_line(canvas, prev_x, prev_y, x, y);
        prev_x = x;
        prev_y = y;
    }
    antenna_check_draw_arrow(canvas, true, "Values");
}

static void antenna_check_draw(Canvas* canvas, void* context) {
    const AntennaCheckModel* model = context;
    canvas_clear(canvas);
    if(model->state == AntennaCheckReady && model->show_sweep) {
        antenna_check_draw_sweep(canvas, model);
        elements_button_center(canvas, "Again");
        elements_button_right(canvas, "Live");
        return;
    }
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 3, 10, "Antenna Check");
    canvas_set_font(canvas, FontSecondary);

    switch(model->state) {
    case AntennaCheckIdle:
        canvas_draw_str(canvas, 3, 32, "Clear both antennas");
        break;
    case AntennaCheckRunning:
        canvas_draw_str(canvas, 3, 28, "Measuring LF and HF...");
        return;
    case AntennaCheckReady: {
        draw_voltage(canvas, 21, "LF 125 kHz", model->lf125_mv);
        draw_voltage(canvas, 31, "LF 134.8 kHz", model->lf134_mv);
        draw_voltage(canvas, 41, "HF 13.56 MHz", model->hf_mv);
        char peak[24];
        antenna_check_peak(peak, sizeof(peak), model->peak_divisor);
        canvas_draw_str(canvas, 3, 51, "LF peak");
        canvas_draw_str_aligned(canvas, 125, 51, AlignRight, AlignBottom, peak);
        antenna_check_draw_arrow(canvas, false, "Plot");
        break;
    }
    case AntennaCheckTimeout:
        canvas_draw_str(canvas, 3, 30, "PM5 did not respond");
        break;
    case AntennaCheckError:
        canvas_draw_str(canvas, 3, 30, "Measurement failed");
        break;
    }

    elements_button_center(canvas, model->state == AntennaCheckIdle ? "Measure" : "Again");
    elements_button_right(canvas, "Live");
}

static int32_t antenna_check_worker(void* context) {
    AntennaCheckPage* page = context;
    PacketResponseNG response;
    AntennaCheckReply reply = {0};
    AntennaCheckState state = AntennaCheckIdle;

    if(proxmark5_frame_begin_operation(&page->cancel)) {
        clearCommandBuffer();
        if(!SendCommandNG(CMD_MEASURE_ANTENNA_TUNING, NULL, 0)) {
            state = AntennaCheckError;
        } else if(!WaitForResponseTimeout(
                      CMD_MEASURE_ANTENNA_TUNING, &response, ANTENNA_CHECK_TIMEOUT_MS)) {
            state = AntennaCheckTimeout;
        } else if(response.status != PM3_SUCCESS || response.length != sizeof(reply)) {
            state = AntennaCheckError;
        } else {
            memcpy(&reply, response.data.asBytes, sizeof(reply));
            state = AntennaCheckReady;
        }
        proxmark5_frame_end_operation();
    }
    AntennaCheckModel* model = view_get_model(page->view);
    model->state = page->cancel ? AntennaCheckIdle : state;
    model->lf125_mv = reply.lf125_mv;
    model->lf134_mv = reply.lf134_mv;
    model->hf_mv = reply.hf_mv;
    model->peak_divisor = reply.peak_divisor;
    memcpy(model->sweep, reply.sweep, sizeof(model->sweep));
    view_commit_model(page->view, true);
    return 0;
}

static bool antenna_check_input(InputEvent* event, void* context) {
    AntennaCheckPage* page = context;
    if(event->type != InputTypeShort) {
        return false;
    }

    if(page->worker && furi_thread_get_state(page->worker) != FuriThreadStateStopped) {
        return event->key != InputKeyBack;
    }
    const bool measure = event->key == InputKeyOk;
    AntennaCheckModel* model = view_get_model(page->view);
    if(model->state == AntennaCheckReady) {
        if(event->key == InputKeyDown && !model->show_sweep) {
            model->show_sweep = true;
            view_commit_model(page->view, true);
            return true;
        }
        if(model->show_sweep && (event->key == InputKeyUp || event->key == InputKeyBack)) {
            model->show_sweep = false;
            view_commit_model(page->view, true);
            return true;
        }
    }
    const bool running = model->state == AntennaCheckRunning;
    if(measure && !running) {
        model->state = AntennaCheckRunning;
    }
    view_commit_model(page->view, measure && !running);

    if(running) {
        return event->key != InputKeyBack;
    }
    if(event->key == InputKeyRight) {
        page->open_live(page->callback_context);
        return true;
    }
    if(!measure) {
        return false;
    }

    if(page->worker) {
        furi_thread_free(page->worker);
    }
    page->cancel = false;
    page->worker = furi_thread_alloc_ex("AntennaCheck", 4096, antenna_check_worker, page);
    furi_thread_start(page->worker);
    return true;
}

static void antenna_check_exit(void* context) {
    AntennaCheckPage* page = context;
    page->cancel = true;
}

AntennaCheckPage* antenna_check_page_create(void (*open_live)(void*), void* context) {
    AntennaCheckPage* page = calloc(1, sizeof(AntennaCheckPage));
    page->open_live = open_live;
    page->callback_context = context;
    page->view = view_alloc();
    view_set_context(page->view, page);
    view_allocate_model(page->view, ViewModelTypeLocking, sizeof(AntennaCheckModel));
    AntennaCheckModel* model = view_get_model(page->view);
    *model = (AntennaCheckModel){.state = AntennaCheckIdle};
    view_commit_model(page->view, false);
    view_set_draw_callback(page->view, antenna_check_draw);
    view_set_input_callback(page->view, antenna_check_input);
    view_set_exit_callback(page->view, antenna_check_exit);
    return page;
}

void antenna_check_page_tick(AntennaCheckPage* page) {
    if(!page->worker || furi_thread_get_state(page->worker) != FuriThreadStateStopped) return;
    furi_thread_free(page->worker);
    page->worker = NULL;
}

void antenna_check_page_stop_worker(AntennaCheckPage* page) {
    if(!page || !page->worker) return;
    page->cancel = true;
    furi_thread_join(page->worker);
    furi_thread_free(page->worker);
    page->worker = NULL;
}

void antenna_check_page_free(AntennaCheckPage* page) {
    antenna_check_page_stop_worker(page);
    view_free_model(page->view);
    view_free(page->view);
    free(page);
}

View* antenna_check_page_get_view(AntennaCheckPage* page) {
    return page->view;
}
