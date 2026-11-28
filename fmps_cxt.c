#include <furi.h>
#include <furi_hal.h>
#include <fmps_cxt_icons.h>
#include "fmps_cxt.h"
#include "proxmark5_com.h"
#include "pages/saved_em4100.h"
#include "pages/saved_ntag213.h"
#include "pages/saved_tag_read.h"

// For GUI:
//  https://github.com/jamisonderek/flipper-zero-tutorials/wiki/User-Interface#viewdisptacher
//  https://brodan.biz/blog/a-visual-guide-to-flipper-zero-gui-components/

// The callback for the back event, it will stop the view dispatcher which will exit the app
static bool fmps_cxt_back_event_callback(void* context) {
    Proxmark5App* app = context;
    view_dispatcher_stop(app->view_dispatcher);
    return true;
}

static void fmps_cxt_open_operate_page(void* context) {
    Proxmark5App* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, OperateOpenMenu);
}

static void fmps_cxt_menu_selected(void* context, uint32_t action) {
    Proxmark5App* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, action);
}

static void fmps_cxt_open_live_voltage(void* context) {
    fmps_cxt_menu_selected(context, OperateOpenLiveVoltage);
}

static void fmps_cxt_open_antenna_check(void* context) {
    fmps_cxt_menu_selected(context, OperateOpenAntennaCheck);
}

static void fmps_cxt_home_selected(void* context, MainPageChoice choice) {
    static const OperateAction actions[] = {
        [MainPageRead] = OperateOpenMenu,
        [MainPageSaved] = OperateOpenSaved,
        [MainPageHardware] = OperateOpenHardware,
    };
    fmps_cxt_menu_selected(context, actions[choice]);
}

static uint32_t saved_record_previous_callback(void* context);

static void open_reader_actions(void* context) {
    fmps_cxt_menu_selected(context, OperateNtag21xActions);
}

static void confirm_ntag21x_write(void* context) {
    fmps_cxt_menu_selected(context, OperateConfirmNtag21x);
}

static void finish_ntag21x_write(void* context) {
    fmps_cxt_menu_selected(context, OperateFinishNtag21x);
}

static bool fmps_cxt_custom_event_callback(void* context, uint32_t event) {
    Proxmark5App* app = context;

    if(event >= OperateSimpleReaderFirst) {
        size_t index = event - OperateSimpleReaderFirst;
        if(index >= simple_reader_count) return false;
        simple_reader_page_start(app->simple_reader_page, simple_readers[index]);
        view_dispatcher_switch_to_view(app->view_dispatcher, SimpleReaderPageViewId);
        return true;
    }

    switch(event) {
    case OperateOpenMenu:
        view_dispatcher_switch_to_view(app->view_dispatcher, OperatePageViewId);
        return true;
    case OperateOpenSaved:
        view_dispatcher_switch_to_view(app->view_dispatcher, SavedMenuViewId);
        return true;
    case OperateBrowseNtag213:
    case OperateImportNtag213: {
        Ntag21xPages pages = {0};
        if(saved_ntag21x_browse(&pages, event == OperateImportNtag213) &&
           simple_reader_page_show_saved(
               app->simple_reader_page, &simple_reader_ntag21x, (const uint8_t*)&pages)) {
            view_dispatcher_switch_to_view(app->view_dispatcher, SimpleReaderPageViewId);
        }
        return true;
    }
    case OperateNtag21xActions:
        submenu_set_selected_item(app->ntag21x_actions, OperateSaveNtag21x);
        view_dispatcher_switch_to_view(app->view_dispatcher, Ntag21xActionsViewId);
        return true;
    case OperateSaveNtag21x: {
        Ntag21xPages pages;
        if(simple_reader_page_get_live_data(
               app->simple_reader_page, &simple_reader_ntag21x, (uint8_t*)&pages)) {
            saved_ntag21x_save(&pages);
        }
        view_dispatcher_switch_to_view(app->view_dispatcher, SimpleReaderPageViewId);
        return true;
    }
    case OperateWriteNtag21x:
    case OperateWriteNtag21xUrl:
    case OperateWriteNtag21xTextAndUrl:
    case OperateWriteNtag21xSmartPoster:
    case OperateWriteNtag21xVCard: {
        Ntag21xPages pages;
        if(simple_reader_page_get_live_data(
               app->simple_reader_page, &simple_reader_ntag21x, (uint8_t*)&pages)) {
            static const Ntag21xWriteFormat formats[] = {
                Ntag21xWriteText, Ntag21xWriteUrl, Ntag21xWriteTextAndUrl,
                Ntag21xWriteSmartPoster, Ntag21xWriteVCard,
            };
            Ntag21xWriteFormat format = formats[event - OperateWriteNtag21x];
            ntag21x_write_page_edit(app->ntag21x_write, &pages, format);
            view_dispatcher_switch_to_view(
                app->view_dispatcher,
                format == Ntag21xWriteText ? Ntag21xTextViewId : Ntag21xUrlViewId);
        }
        return true;
    }
    case OperateConfirmNtag21x:
        ntag21x_write_page_confirm(app->ntag21x_write);
        view_dispatcher_switch_to_view(app->view_dispatcher, Ntag21xWriteViewId);
        return true;
    case OperateFinishNtag21x:
        ntag21x_write_page_wait(app->ntag21x_write);
        simple_reader_page_start(app->simple_reader_page, &simple_reader_ntag21x);
        view_dispatcher_switch_to_view(app->view_dispatcher, SimpleReaderPageViewId);
        return true;
    case OperateBrowseEm4100:
        saved_em4100_browse();
        return true;
    case OperateBrowseTagReads:
        if(saved_tag_read_browse(app->saved_tag_text)) {
            view_set_previous_callback(text_box_get_view(app->saved_tag_view), saved_record_previous_callback);
            text_box_reset(app->saved_tag_view);
            text_box_set_text(app->saved_tag_view, furi_string_get_cstr(app->saved_tag_text));
            view_dispatcher_switch_to_view(app->view_dispatcher, SavedTagReadViewId);
        }
        return true;
    case OperateOpenHardware:
        view_dispatcher_switch_to_view(app->view_dispatcher, HardwareMenuViewId);
        return true;
    case OperateReadHitag2:
        view_dispatcher_switch_to_view(app->view_dispatcher, ReadHitag2PageViewId);
        read_hitag2_page_start(app->read_hitag2_page);
        return true;
    case OperateReadHf14a:
        view_dispatcher_switch_to_view(app->view_dispatcher, ReadHf14aPageViewId);
        read_hf14a_page_start(app->read_hf14a_page);
        return true;
    case OperateOpenStatus:
        view_dispatcher_switch_to_view(app->view_dispatcher, StatusPageViewId);
        status_page_start(app->status_page);
        return true;
    case OperateOpenAntennaCheck:
        view_dispatcher_switch_to_view(app->view_dispatcher, AntennaCheckPageViewId);
        return true;
    case OperateOpenLiveVoltage:
        view_dispatcher_switch_to_view(app->view_dispatcher, LiveVoltagePageViewId);
        return true;
    default:
        return false;
    }
}

static uint32_t operate_page_previous_callback(void* context) {
    UNUSED(context);
    return MainPageViewId;
}

// Context is the view's context, which is the page itself (set in read_hitag2_page_create).
static uint32_t read_hitag2_page_previous_callback(void* context) {
    ReadHitag2Page* read_hitag2_page = context;
    read_hitag2_page_stop(read_hitag2_page);
    return OperatePageViewId;
}

// Context is the view's context, which is the page itself (set in read_hf14a_page_create).
static uint32_t read_hf14a_page_previous_callback(void* context) {
    ReadHf14aPage* read_hf14a_page = context;
    read_hf14a_page_stop(read_hf14a_page);
    return OperatePageViewId;
}

static void fmps_cxt_tick(void* context) {
    Proxmark5App* app = context;
    read_hitag2_page_tick(app->read_hitag2_page);
    read_hf14a_page_tick(app->read_hf14a_page);
    antenna_check_page_tick(app->antenna_check_page);
    live_voltage_page_tick(app->live_voltage_page);
    status_page_tick(app->status_page);
    simple_reader_page_tick(app->simple_reader_page);
}

static uint32_t status_page_previous_callback(void* context) {
    StatusPage* status_page = context;
    status_page_stop(status_page);
    return HardwareMenuViewId;
}

static uint32_t hardware_menu_previous_callback(void* context) {
    UNUSED(context);
    return HardwareMenuViewId;
}

static uint32_t live_voltage_previous_callback(void* context) {
    UNUSED(context);
    return AntennaCheckPageViewId;
}

static uint32_t saved_record_previous_callback(void* context) {
    UNUSED(context);
    return SavedMenuViewId;
}

static uint32_t reader_details_previous_callback(void* context) {
    UNUSED(context);
    return SimpleReaderPageViewId;
}

static void open_reader_details(void* context) {
    Proxmark5App* app = context;
    if(!simple_reader_page_format_details(app->simple_reader_page, app->saved_tag_text)) return;
    text_box_reset(app->saved_tag_view);
    text_box_set_text(app->saved_tag_view, furi_string_get_cstr(app->saved_tag_text));
    View* details = text_box_get_view(app->saved_tag_view);
    view_set_previous_callback(details, reader_details_previous_callback);
    view_dispatcher_switch_to_view(app->view_dispatcher, SavedTagReadViewId);
}

static uint32_t ntag21x_text_previous(void* context) {
    UNUSED(context);
    return Ntag21xActionsViewId;
}

static uint32_t ntag21x_write_previous(void* context) {
    ntag21x_write_page_wait(context);
    return ntag21x_write_page_is_url(context) ? Ntag21xUrlViewId : SimpleReaderPageViewId;
}

static uint32_t simple_reader_previous_callback(void* context) {
    bool saved = simple_reader_page_is_saved(context);
    simple_reader_page_stop(context);
    return saved ? SavedMenuViewId : OperatePageViewId;
}

Proxmark5App* proxmark5_app_alloc() {
    Proxmark5App* app = calloc(1, sizeof(Proxmark5App));
    app->gui = furi_record_open(RECORD_GUI);

    // Allocate the view dispatcher and attach it to the GUI
    app->view_dispatcher = view_dispatcher_alloc();
    // Attach the view dispatcher to the GUI with fullscreen type
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    // Create and add the main page view to the view dispatcher
    app->main_page = main_page_create(fmps_cxt_home_selected, app);
    view_dispatcher_add_view(app->view_dispatcher, MainPageViewId, app->main_page->canvas_view);

    // Create and add the operate submenu page to the view dispatcher
    app->operate_page = operate_page_create(fmps_cxt_menu_selected, app);
    view_set_previous_callback(
        submenu_get_view(app->operate_page->submenu), operate_page_previous_callback);
    view_dispatcher_add_view(
        app->view_dispatcher, OperatePageViewId, submenu_get_view(app->operate_page->submenu));

    View* hardware_menu = submenu_get_view(app->operate_page->hardware_submenu);
    view_set_previous_callback(hardware_menu, operate_page_previous_callback);
    view_dispatcher_add_view(app->view_dispatcher, HardwareMenuViewId, hardware_menu);

    View* saved_menu = submenu_get_view(app->operate_page->saved_submenu);
    view_set_previous_callback(saved_menu, operate_page_previous_callback);
    view_dispatcher_add_view(app->view_dispatcher, SavedMenuViewId, saved_menu);
    app->saved_tag_text = furi_string_alloc();
    app->saved_tag_view = text_box_alloc();
    View* saved_result = text_box_get_view(app->saved_tag_view);
    view_set_previous_callback(saved_result, saved_record_previous_callback);
    view_dispatcher_add_view(app->view_dispatcher, SavedTagReadViewId, saved_result);

    app->simple_reader_page = simple_reader_page_create(open_reader_details, open_reader_actions, app);
    View* reader = simple_reader_page_get_view(app->simple_reader_page);
    view_set_previous_callback(reader, simple_reader_previous_callback);
    view_dispatcher_add_view(app->view_dispatcher, SimpleReaderPageViewId, reader);

    app->ntag21x_actions = submenu_alloc();
    submenu_set_header(app->ntag21x_actions, "NTAG21x");
    submenu_add_item(app->ntag21x_actions, "Save dump", OperateSaveNtag21x, fmps_cxt_menu_selected, app);
    submenu_add_item(app->ntag21x_actions, "Write NDEF text", OperateWriteNtag21x, fmps_cxt_menu_selected, app);
    submenu_add_item(app->ntag21x_actions, "Write NDEF URL", OperateWriteNtag21xUrl, fmps_cxt_menu_selected, app);
    submenu_add_item(
        app->ntag21x_actions, "Write text + URL", OperateWriteNtag21xTextAndUrl,
        fmps_cxt_menu_selected, app);
    submenu_add_item(
        app->ntag21x_actions, "Write Smart Poster", OperateWriteNtag21xSmartPoster,
        fmps_cxt_menu_selected, app);
    submenu_add_item(
        app->ntag21x_actions, "Write vCard", OperateWriteNtag21xVCard, fmps_cxt_menu_selected, app);
    View* actions = submenu_get_view(app->ntag21x_actions);
    view_set_previous_callback(actions, reader_details_previous_callback);
    view_dispatcher_add_view(app->view_dispatcher, Ntag21xActionsViewId, actions);

    app->ntag21x_write = ntag21x_write_page_alloc(confirm_ntag21x_write, finish_ntag21x_write, app);
    View* text_input = ntag21x_write_page_get_input_view(app->ntag21x_write);
    view_set_previous_callback(text_input, ntag21x_text_previous);
    view_dispatcher_add_view(app->view_dispatcher, Ntag21xTextViewId, text_input);
    View* url_input = ntag21x_write_page_get_url_view(app->ntag21x_write);
    view_set_previous_callback(url_input, ntag21x_text_previous);
    view_dispatcher_add_view(app->view_dispatcher, Ntag21xUrlViewId, url_input);
    View* write_view = ntag21x_write_page_get_view(app->ntag21x_write);
    view_set_previous_callback(write_view, ntag21x_write_previous);
    view_dispatcher_add_view(app->view_dispatcher, Ntag21xWriteViewId, write_view);

    // Create and add ReadHitag2 page to the view dispatcher
    // The left "Back" button returns to the functions menu, same destination as the
    // hardware back button below.
    app->read_hitag2_page = read_hitag2_page_create(fmps_cxt_open_operate_page, app);
    view_set_previous_callback(
        read_hitag2_page_get_view(app->read_hitag2_page), read_hitag2_page_previous_callback);
    view_dispatcher_add_view(
        app->view_dispatcher,
        ReadHitag2PageViewId,
        read_hitag2_page_get_view(app->read_hitag2_page));

    // Create and add HF 14a Reader page to the view dispatcher - same Back
    // button/destination convention as ReadHitag2 above.
    app->read_hf14a_page = read_hf14a_page_create(fmps_cxt_open_operate_page, app);
    view_set_previous_callback(
        read_hf14a_page_get_view(app->read_hf14a_page), read_hf14a_page_previous_callback);
    view_dispatcher_add_view(
        app->view_dispatcher,
        ReadHf14aPageViewId,
        read_hf14a_page_get_view(app->read_hf14a_page));

    // Create and add the hardware-status dashboard to the view dispatcher.
    app->status_page = status_page_create();
    view_set_previous_callback(
        status_page_get_view(app->status_page), status_page_previous_callback);
    view_dispatcher_add_view(
        app->view_dispatcher, StatusPageViewId, status_page_get_view(app->status_page));

    app->antenna_check_page = antenna_check_page_create(fmps_cxt_open_live_voltage, app);
    View* antenna_check_view = antenna_check_page_get_view(app->antenna_check_page);
    view_set_previous_callback(antenna_check_view, hardware_menu_previous_callback);
    view_dispatcher_add_view(app->view_dispatcher, AntennaCheckPageViewId, antenna_check_view);

    app->live_voltage_page = live_voltage_page_create(fmps_cxt_open_antenna_check, app);
    View* live_voltage_view = live_voltage_page_get_view(app->live_voltage_page);
    view_set_previous_callback(live_voltage_view, live_voltage_previous_callback);
    view_dispatcher_add_view(app->view_dispatcher, LiveVoltagePageViewId, live_voltage_view);

    // Set the back event callback to handle the back button press, and pass the app context to it
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, fmps_cxt_back_event_callback);
    view_dispatcher_set_custom_event_callback(
        app->view_dispatcher, fmps_cxt_custom_event_callback);
    view_dispatcher_set_tick_event_callback(app->view_dispatcher, fmps_cxt_tick, furi_ms_to_ticks(50));
    return app;
}

void proxmark5_app_free(Proxmark5App* app) {
    furi_assert(app);

    // Workers can still update their views after the dispatcher loop stops.
    main_page_stop_worker(app->main_page);
    read_hitag2_page_stop_worker(app->read_hitag2_page);
    read_hf14a_page_stop_worker(app->read_hf14a_page);
    status_page_stop_worker(app->status_page);
    antenna_check_page_stop_worker(app->antenna_check_page);
    live_voltage_page_stop_worker(app->live_voltage_page);
    ntag21x_write_page_wait(app->ntag21x_write);
    simple_reader_page_stop_worker(app->simple_reader_page);

    view_dispatcher_remove_view(app->view_dispatcher, Ntag21xActionsViewId);
    view_dispatcher_remove_view(app->view_dispatcher, Ntag21xTextViewId);
    view_dispatcher_remove_view(app->view_dispatcher, Ntag21xUrlViewId);
    view_dispatcher_remove_view(app->view_dispatcher, Ntag21xWriteViewId);
    submenu_free(app->ntag21x_actions);
    ntag21x_write_page_free(app->ntag21x_write);
    view_dispatcher_remove_view(app->view_dispatcher, SimpleReaderPageViewId);
    simple_reader_page_free(app->simple_reader_page);
    view_dispatcher_remove_view(app->view_dispatcher, SavedTagReadViewId);
    text_box_free(app->saved_tag_view);
    furi_string_free(app->saved_tag_text);
    view_dispatcher_remove_view(app->view_dispatcher, SavedMenuViewId);

    view_dispatcher_remove_view(app->view_dispatcher, MainPageViewId);
    view_dispatcher_remove_view(app->view_dispatcher, OperatePageViewId);
    view_dispatcher_remove_view(app->view_dispatcher, HardwareMenuViewId);
    view_dispatcher_remove_view(app->view_dispatcher, ReadHitag2PageViewId);
    view_dispatcher_remove_view(app->view_dispatcher, ReadHf14aPageViewId);
    view_dispatcher_remove_view(app->view_dispatcher, StatusPageViewId);
    if(app->main_page) {
        main_page_free(app->main_page);
        app->main_page = NULL;
    }
    if(app->operate_page) {
        operate_page_free(app->operate_page);
        app->operate_page = NULL;
    }
    if(app->read_hitag2_page) {
        read_hitag2_page_free(app->read_hitag2_page);
        app->read_hitag2_page = NULL;
    }
    if(app->read_hf14a_page) {
        read_hf14a_page_free(app->read_hf14a_page);
        app->read_hf14a_page = NULL;
    }
    if(app->status_page) {
        status_page_free(app->status_page);
        app->status_page = NULL;
    }
    view_dispatcher_remove_view(app->view_dispatcher, AntennaCheckPageViewId);
    antenna_check_page_free(app->antenna_check_page);
    view_dispatcher_remove_view(app->view_dispatcher, LiveVoltagePageViewId);
    live_voltage_page_free(app->live_voltage_page);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);
    free(app);
}

// Furi application entry point
int32_t fmps_cxt_app(void* p) {
    UNUSED(p);

    // Initialize proxmark5 communication interfaces
    proxmark5_com_init();

    // Create the app context and start the view dispatcher
    Proxmark5App* app = proxmark5_app_alloc();
    // Default to the main page view
    view_dispatcher_switch_to_view(app->view_dispatcher, MainPageViewId);
    main_page_start(app->main_page);
    view_dispatcher_run(app->view_dispatcher);
    // Free the app context and exit
    proxmark5_app_free(app);

    // Deinitialize proxmark5 communication interfaces
    proxmark5_com_deinit();

    return 0;
}
