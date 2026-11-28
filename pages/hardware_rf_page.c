#include <furi.h>
#include <gui/elements.h>
#include "hardware_rf_page.h"
#include "../readers/hardware_rf.h"
#include "proxmark5_frame.h"

typedef enum {
    RfReadSetting,
    RfApplySetting,
    RfRunTest,
} RfAction;

typedef struct {
    HardwareRfMode mode;
    RfAction action;
    HardwareRfTest test;
    uint16_t current;
    uint16_t target;
    int status;
    bool running;
    bool finished;
    bool have_value;
} HardwareRfModel;

struct HardwareRfPage {
    View* view;
    FuriThread* worker;
    volatile bool cancel;
};

static bool rf_is_setting(HardwareRfMode mode) {
    return mode == HardwareRfThreshold || mode == HardwareRfFrequency;
}

static void draw_setting(Canvas* canvas, const char* label, HardwareRfMode mode, uint16_t value, uint8_t y) {
    char line[32];
    if(mode == HardwareRfFrequency) {
        uint32_t rate = (120000U + (value + 1U) / 2U) / (value + 1U);
        snprintf(line, sizeof(line), "%s %lu.%lu kHz", label, (unsigned long)(rate / 10), (unsigned long)(rate % 10));
    } else {
        snprintf(line, sizeof(line), "%s %u", label, value);
    }
    canvas_draw_str(canvas, 4, y, line);
}

static void draw_counts(Canvas* canvas, const HardwareRfTest* test) {
    char line[24];
    snprintf(line, sizeof(line), "Selected  %u/%u", test->selected, HARDWARE_RF_TEST_ATTEMPTS);
    canvas_draw_str(canvas, 4, 22, line);
    snprintf(line, sizeof(line), "GET VERSION  %u/%u", test->version, HARDWARE_RF_TEST_ATTEMPTS);
    canvas_draw_str(canvas, 4, 34, line);
    snprintf(line, sizeof(line), "Read 4 pages  %u/%u", test->read, HARDWARE_RF_TEST_ATTEMPTS);
    canvas_draw_str(canvas, 4, 46, line);
}

static void rf_page_draw(Canvas* canvas, void* context) {
    static const char* const titles[] = {
        [HardwareRfThreshold] = "14A RX Threshold",
        [HardwareRfFrequency] = "LF Sampling",
        [HardwareRfReadTest] = "14A Read Test",
    };
    const HardwareRfModel* model = context;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 4, 11, titles[model->mode]);
    canvas_set_font(canvas, FontSecondary);
    if(model->running) {
        canvas_draw_str(canvas, 4, 32, model->action == RfRunTest ? "Testing 5 reads..." : "Waiting for PM5...");
        return;
    }
    if(model->finished && model->status != PM3_SUCCESS) {
        if(model->status == PM3_EDEVNOTSUPP && model->mode == HardwareRfThreshold) {
            canvas_draw_str(canvas, 4, 29, "Threshold unavailable");
            canvas_draw_str(canvas, 4, 42, "Update PM5 firmware");
        } else if(model->status == PM3_EOPABORTED) {
            canvas_draw_str(canvas, 4, 32, "Cancelled");
        } else if(model->status == HardwareRfFieldOffUnconfirmed) {
            canvas_draw_str(canvas, 4, 32, "Field off unconfirmed");
        } else if(model->mode == HardwareRfReadTest) {
            canvas_draw_str(canvas, 4, 32, "Read test interrupted");
        } else {
            canvas_draw_str(canvas, 4, 32, "PM5 setting unconfirmed");
        }
        elements_button_center(canvas, "Retry");
        return;
    }
    if(rf_is_setting(model->mode) && model->have_value) {
        draw_setting(canvas, "Current", model->mode, model->current, 24);
        draw_setting(canvas, "Set", model->mode, model->target, 37);
        if(model->action == RfApplySetting) canvas_draw_str(canvas, 4, 49, "Applied");
        elements_button_left(canvas, model->mode == HardwareRfFrequency ? "125" : "-");
        elements_button_right(canvas, model->mode == HardwareRfFrequency ? "134.8" : "+");
        elements_button_center(canvas, model->target != model->current ? "Apply" : "Refresh");
    } else if(model->mode == HardwareRfReadTest && model->finished) {
        if(model->test.selected) draw_counts(canvas, &model->test);
        else canvas_draw_str(canvas, 4, 32, "No tag detected (0/5)");
        elements_button_center(canvas, "Again");
    } else if(model->mode == HardwareRfReadTest) {
        canvas_draw_str(canvas, 4, 29, "Place tag on HF coil");
        canvas_draw_str(canvas, 4, 42, "5 tries, read only");
        elements_button_center(canvas, "Start");
    }
}

static int32_t rf_worker(void* context) {
    HardwareRfPage* page = context;
    HardwareRfModel* model = view_get_model(page->view);
    const RfAction action = model->action;
    const HardwareRfMode mode = model->mode;
    uint16_t value = model->target;
    view_commit_model(page->view, false);

    HardwareRfTest test = {0};
    int status = PM3_EOPABORTED;
    if(proxmark5_frame_begin_operation(&page->cancel)) {
        clearCommandBuffer();
        if(action == RfRunTest) {
            status = hardware_rf_read_test(&test, &page->cancel);
        } else if(mode == HardwareRfFrequency) {
            status = hardware_rf_lf_divisor(action == RfApplySetting, &value);
        } else {
            uint8_t threshold = value;
            status = hardware_rf_threshold(action == RfApplySetting, &threshold, &page->cancel);
            value = threshold;
        }
        proxmark5_frame_end_operation();
    }

    model = view_get_model(page->view);
    model->running = false;
    model->finished = true;
    model->status = status;
    model->test = test;
    if(rf_is_setting(mode)) {
        model->have_value = status == PM3_SUCCESS;
        if(model->have_value) {
            model->current = value;
            model->target = mode == HardwareRfThreshold ? MAX(value, HARDWARE_RF_THRESHOLD_MIN) : value;
        }
    }
    view_commit_model(page->view, true);
    return 0;
}

static void rf_start(HardwareRfPage* page, RfAction action) {
    if(page->worker) {
        furi_thread_join(page->worker);
        furi_thread_free(page->worker);
    }
    HardwareRfModel* model = view_get_model(page->view);
    model->action = action;
    model->running = true;
    view_commit_model(page->view, true);
    page->cancel = false;
    page->worker = furi_thread_alloc_ex("HardwareRf", 4096, rf_worker, page);
    furi_thread_start(page->worker);
}

static bool rf_page_input(InputEvent* event, void* context) {
    const bool horizontal = event->key == InputKeyLeft || event->key == InputKeyRight;
    if(event->type != InputTypeShort && (event->type != InputTypeRepeat || !horizontal)) return false;
    HardwareRfPage* page = context;
    HardwareRfModel* model = view_get_model(page->view);
    if(model->running) {
        if(event->key == InputKeyBack) page->cancel = true;
        view_commit_model(page->view, false);
        return true;
    }
    const bool adjust = horizontal && model->have_value &&
                        (model->mode == HardwareRfThreshold || event->type == InputTypeShort);
    if(adjust) {
        const bool right = event->key == InputKeyRight;
        uint16_t target;
        if(model->mode == HardwareRfThreshold) {
            const int step = right ? 1 : -1;
            target = CLAMP(
                (int)model->target + step, HARDWARE_RF_THRESHOLD_MAX, HARDWARE_RF_THRESHOLD_MIN);
        } else {
            target = right ? LF_DIVISOR_134 : LF_DIVISOR_125;
        }
        const bool changed = target != model->target;
        if(changed) {
            model->target = target;
            model->action = RfReadSetting;
        }
        view_commit_model(page->view, changed);
        return true;
    }
    RfAction action = RfReadSetting;
    if(model->mode == HardwareRfReadTest) action = RfRunTest;
    else if(model->have_value && model->target != model->current) action = RfApplySetting;
    view_commit_model(page->view, false);
    if(event->key == InputKeyOk) {
        rf_start(page, action);
        return true;
    }
    return false;
}

HardwareRfPage* hardware_rf_page_alloc(void) {
    HardwareRfPage* page = calloc(1, sizeof(HardwareRfPage));
    page->view = view_alloc();
    view_set_context(page->view, page);
    view_allocate_model(page->view, ViewModelTypeLocking, sizeof(HardwareRfModel));
    view_set_draw_callback(page->view, rf_page_draw);
    view_set_input_callback(page->view, rf_page_input);
    return page;
}

View* hardware_rf_page_view(HardwareRfPage* page) {
    return page->view;
}

void hardware_rf_page_open(HardwareRfPage* page, HardwareRfMode mode) {
    HardwareRfModel* model = view_get_model(page->view);
    *model = (HardwareRfModel){.mode = mode};
    view_commit_model(page->view, true);
    if(rf_is_setting(mode)) rf_start(page, RfReadSetting);
}

void hardware_rf_page_stop_worker(HardwareRfPage* page) {
    if(page->worker) {
        page->cancel = true;
        furi_thread_join(page->worker);
        furi_thread_free(page->worker);
        page->worker = NULL;
    }
}

void hardware_rf_page_free(HardwareRfPage* page) {
    hardware_rf_page_stop_worker(page);
    view_free(page->view);
    free(page);
}
