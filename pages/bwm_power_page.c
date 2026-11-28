#include <furi.h>
#include <gui/elements.h>
#include <string.h>

#include "bwm_power_page.h"
#include "proxmark5_frame.h"

#define BWM_POWER_TIMEOUT_MS 8000

typedef struct {
    const char* name;
    const char* help;
} BwmPowerChoice;

typedef struct {
    uint16_t command;
    const char* label;
    const BwmPowerChoice* choices;
    uint8_t choice_count;
    uint8_t reply_size;
} BwmPowerSetting;

typedef struct {
    int status;
    uint8_t value;
} BwmPowerValue;

static const BwmPowerChoice bwm_save_choices[] = {
    {"Off", "Keep module awake"},
    {"On", "Sleep between traffic"},
};

static const BwmPowerChoice wifi_sleep_choices[] = {
    {"None", "Keep Wi-Fi awake"},
    {"Min", "Sleep between beacons"},
    {"Max", "Use listen interval"},
};

static const BwmPowerSetting bwm_power_settings[] = {
    {CMD_PM5_BWM_POWERSAVE, "Power save", bwm_save_choices, COUNT_OF(bwm_save_choices), 1},
    {CMD_PM5_BWM_WIFI_PS, "Wi-Fi sleep", wifi_sleep_choices, COUNT_OF(wifi_sleep_choices), 2},
};

typedef enum {
    BwmPowerReading,
    BwmPowerReady,
    BwmPowerEditing,
    BwmPowerApplying,
} BwmPowerStage;

typedef struct {
    BwmPowerStage stage;
    uint8_t selected;
    uint8_t pending;
    bool applied;
    bool apply_uncertain;
    BwmPowerValue values[COUNT_OF(bwm_power_settings)];
} BwmPowerModel;

struct BwmPowerPage {
    View* view;
    FuriThread* worker;
    volatile bool cancel;
    bool pending;
};

static BwmPowerValue bwm_power_request(const BwmPowerSetting* setting, bool apply, uint8_t wanted) {
    uint8_t payload[] = {apply ? BWM_POWER_ACTION_SET : BWM_POWER_ACTION_GET, wanted};
    PacketResponseNG reply;
    clearCommandBuffer();
    if(!SendCommandNG(setting->command, payload, apply ? sizeof(payload) : 1)) {
        return (BwmPowerValue){.status = PM3_EIO};
    }
    if(!WaitForResponseTimeout(setting->command, &reply, BWM_POWER_TIMEOUT_MS)) {
        return (BwmPowerValue){.status = PM3_ETIMEOUT};
    }
    if(reply.status != PM3_SUCCESS) return (BwmPowerValue){.status = reply.status};
    if(reply.length != setting->reply_size || reply.data.asBytes[0] >= setting->choice_count) {
        return (BwmPowerValue){.status = PM3_ESOFT};
    }
    if(apply && reply.data.asBytes[0] != wanted) return (BwmPowerValue){.status = PM3_ESOFT};
    return (BwmPowerValue){
        .status = PM3_SUCCESS,
        .value = reply.data.asBytes[0],
    };
}

static const char* bwm_power_value_text(const BwmPowerSetting* setting, BwmPowerValue value) {
    switch(value.status) {
    case PM3_SUCCESS:
        return setting->choices[value.value].name;
    case PM3_ENOTIMPL:
    case PM3_EDEVNOTSUPP:
        return "N/A";
    case PM3_ETIMEOUT:
        return "No reply";
    default:
        return "Error";
    }
}

static void bwm_power_draw(Canvas* canvas, void* context) {
    const BwmPowerModel* model = context;
    const BwmPowerSetting* selected = &bwm_power_settings[model->selected];
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 4, 10, "BWM Power");
    canvas_set_font(canvas, FontSecondary);

    if(model->stage == BwmPowerReading || model->stage == BwmPowerApplying) {
        canvas_draw_str(canvas, 4, 30, model->stage == BwmPowerReading ? "Reading settings..." :
                                                                       "Applying setting...");
        return;
    }
    if(model->stage == BwmPowerEditing) {
        canvas_draw_str(canvas, 4, 24, selected->label);
        const BwmPowerChoice* choice = &selected->choices[model->pending];
        canvas_draw_str_aligned(canvas, 64, 36, AlignCenter, AlignBottom, choice->name);
        canvas_draw_str_aligned(canvas, 64, 48, AlignCenter, AlignBottom, choice->help);
        elements_button_left(canvas, "Prev");
        elements_button_center(canvas, "Apply");
        elements_button_right(canvas, "Next");
        return;
    }

    for(size_t i = 0; i < COUNT_OF(bwm_power_settings); i++) {
        uint8_t y = 25 + 12 * i;
        if(i == model->selected) canvas_draw_str(canvas, 4, y, ">");
        canvas_draw_str(canvas, 13, y, bwm_power_settings[i].label);
        canvas_draw_str_aligned(
            canvas, 124, y, AlignRight, AlignBottom,
            bwm_power_value_text(&bwm_power_settings[i], model->values[i]));
    }
    if(model->apply_uncertain) {
        canvas_draw_str(canvas, 4, 48, "Refresh to check setting");
    } else if(model->applied) {
        canvas_draw_str(canvas, 4, 48, "Setting applied");
    }

    if(model->values[model->selected].status == PM3_SUCCESS) {
        elements_button_center(canvas, "Edit");
    }
    elements_button_right(canvas, "Refresh");
}

static int32_t bwm_power_worker(void* context) {
    BwmPowerPage* page = context;
    BwmPowerModel* model = view_get_model(page->view);
    bool apply = model->stage == BwmPowerApplying;
    uint8_t selected = model->selected;
    uint8_t wanted = model->pending;
    view_commit_model(page->view, false);

    BwmPowerValue values[COUNT_OF(bwm_power_settings)] = {0};
    if(!proxmark5_frame_begin_operation(&page->cancel)) return 0;
    if(apply) {
        const BwmPowerSetting* setting = &bwm_power_settings[selected];
        BwmPowerValue reply = bwm_power_request(setting, true, wanted);
        values[selected] = reply.status == PM3_SUCCESS ? bwm_power_request(setting, false, 0) :
                                                         reply;
    } else {
        for(size_t i = 0; i < COUNT_OF(bwm_power_settings) && !page->cancel; i++) {
            values[i] = bwm_power_request(&bwm_power_settings[i], false, 0);
        }
    }

    proxmark5_frame_end_operation();
    model = view_get_model(page->view);
    if(page->cancel) {
        view_commit_model(page->view, false);
        return 0;
    }
    if(apply) {
        model->values[selected] = values[selected];
    } else {
        memcpy(model->values, values, sizeof(values));
    }
    model->applied = apply && values[selected].status == PM3_SUCCESS &&
                     values[selected].value == wanted;
    model->apply_uncertain = apply && !model->applied;
    model->stage = BwmPowerReady;
    view_commit_model(page->view, true);
    return 0;
}

static void bwm_power_start_worker(BwmPowerPage* page) {
    if(!page->worker) {
        page->worker = furi_thread_alloc_ex("BwmPower", 4096, bwm_power_worker, page);
    }
    furi_thread_start(page->worker);
}

static bool bwm_power_input(InputEvent* event, void* context) {
    BwmPowerPage* page = context;
    if(event->type != InputTypeShort) return false;
    BwmPowerModel* model = view_get_model(page->view);
    if(model->stage == BwmPowerApplying) {
        view_commit_model(page->view, false);
        return true;
    }
    if(model->stage == BwmPowerReading ||
       (page->worker && furi_thread_get_state(page->worker) != FuriThreadStateStopped)) {
        view_commit_model(page->view, false);
        return event->key != InputKeyBack;
    }

    InputKey key = event->key;
    bool handled = true;
    bool start_worker = false;
    uint8_t selected = model->selected;
    if(model->stage == BwmPowerEditing) {
        uint8_t count = bwm_power_settings[selected].choice_count;
        if(key == InputKeyLeft || key == InputKeyRight) {
            model->pending = (model->pending + (key == InputKeyLeft ? count - 1 : 1)) % count;
        } else if(key == InputKeyBack) {
            model->stage = BwmPowerReady;
        } else if(key == InputKeyOk) {
            start_worker = model->pending != model->values[selected].value;
            model->stage = start_worker ? BwmPowerApplying : BwmPowerReady;
        } else {
            handled = false;
        }
    } else if(key == InputKeyUp || key == InputKeyDown) {
        size_t count = COUNT_OF(bwm_power_settings);
        model->selected = (selected + (key == InputKeyUp ? count - 1 : 1)) % count;
    } else if(key == InputKeyRight) {
        model->stage = BwmPowerReading;
        start_worker = true;
    } else if(key == InputKeyOk && model->values[selected].status == PM3_SUCCESS) {
        model->pending = model->values[selected].value;
        model->stage = BwmPowerEditing;
    } else {
        handled = false;
    }
    view_commit_model(page->view, handled);
    if(start_worker) bwm_power_start_worker(page);
    return handled;
}

static void bwm_power_exit(void* context) {
    BwmPowerPage* page = context;
    page->cancel = true;
    page->pending = false;
}

BwmPowerPage* bwm_power_page_create(void) {
    BwmPowerPage* page = calloc(1, sizeof(BwmPowerPage));
    page->view = view_alloc();
    view_allocate_model(page->view, ViewModelTypeLocking, sizeof(BwmPowerModel));
    view_set_context(page->view, page);
    view_set_draw_callback(page->view, bwm_power_draw);
    view_set_input_callback(page->view, bwm_power_input);
    view_set_exit_callback(page->view, bwm_power_exit);
    BwmPowerModel* model = view_get_model(page->view);
    *model = (BwmPowerModel){.stage = BwmPowerReading};
    view_commit_model(page->view, false);
    return page;
}

void bwm_power_page_stop_worker(BwmPowerPage* page) {
    if(!page || !page->worker) return;
    bwm_power_exit(page);
    furi_thread_join(page->worker);
    furi_thread_free(page->worker);
    page->worker = NULL;
}

void bwm_power_page_free(BwmPowerPage* page) {
    bwm_power_page_stop_worker(page);
    view_free(page->view);
    free(page);
}

View* bwm_power_page_get_view(BwmPowerPage* page) {
    return page->view;
}

void bwm_power_page_tick(BwmPowerPage* page) {
    if(page->worker) {
        if(furi_thread_get_state(page->worker) != FuriThreadStateStopped) return;
        furi_thread_free(page->worker);
        page->worker = NULL;
    }
    if(!page->pending) return;
    page->pending = false;
    page->cancel = false;
    bwm_power_start_worker(page);
}

void bwm_power_page_start(BwmPowerPage* page) {
    BwmPowerModel* model = view_get_model(page->view);
    model->stage = BwmPowerReading;
    view_commit_model(page->view, true);
    page->pending = true;
    bwm_power_page_tick(page);
}
