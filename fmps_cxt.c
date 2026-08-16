#include <furi.h>
#include <furi_hal.h>
#include <storage/storage.h>
#include <loader/loader.h>
#include <fmps_cxt_icons.h>
#include "fmps_cxt.h"
#include "proxmark5_com.h"

#define MFKEY_FAP_NFC  EXT_PATH("apps/NFC/mfkey.fap")
#define MFKEY_FAP_RFID EXT_PATH("apps/RFID/mfkey.fap")

static Proxmark5App* s_app = NULL;

static bool fmps_cxt_back_event_callback(void* context) {
    Proxmark5App* app = context;
    view_dispatcher_stop(app->view_dispatcher);
    return true;
}

/** Quit Proxmark5 then open MFKey (nonces already in /ext/nfc/.nested.log). */
static void fmps_cxt_launch_mfkey(void* context) {
    Proxmark5App* app = context;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    const char* path = MFKEY_FAP_NFC;
    if(!storage_file_exists(storage, path)) {
        path = MFKEY_FAP_RFID;
    }
    bool exists = storage_file_exists(storage, path);
    furi_record_close(RECORD_STORAGE);

    if(exists) {
        Loader* loader = furi_record_open(RECORD_LOADER);
        loader_enqueue_launch(loader, path, NULL, LoaderDeferredLaunchFlagGui);
        furi_record_close(RECORD_LOADER);
    }
    view_dispatcher_stop(app->view_dispatcher);
}

static void fmps_cxt_open_operate_page(void* context) {
    Proxmark5App* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, Proxmark5CustomEventOpenOperatePage);
}

static void fmps_cxt_navigate_group(void* context, OperateRootIndex group) {
    Proxmark5App* app = context;
    if(group == OperateRootTools) {
        view_dispatcher_send_custom_event(
            app->view_dispatcher, Proxmark5CustomEventOpenToolsMenu);
    } else if(group == OperateRootHf) {
        view_dispatcher_send_custom_event(app->view_dispatcher, Proxmark5CustomEventOpenHfMenu);
    }
}

static void fmps_cxt_operate_item(void* context, uint32_t index) {
    Proxmark5App* app = context;
    switch(index) {
    case OperateItemPing:
        app->pending_simple_cmd = SimpleCmdPing;
        app->leaf_back = LeafBackToTools;
        view_dispatcher_send_custom_event(
            app->view_dispatcher, Proxmark5CustomEventOpenSimpleCmdPage);
        break;
    case OperateItemVersion:
        app->pending_simple_cmd = SimpleCmdVersion;
        app->leaf_back = LeafBackToTools;
        view_dispatcher_send_custom_event(
            app->view_dispatcher, Proxmark5CustomEventOpenSimpleCmdPage);
        break;
    case OperateItemHf14a:
        app->pending_simple_cmd = SimpleCmdHf14a;
        app->leaf_back = LeafBackToHf;
        view_dispatcher_send_custom_event(
            app->view_dispatcher, Proxmark5CustomEventOpenSimpleCmdPage);
        break;
    case OperateItemAutopwn:
        app->leaf_back = LeafBackToHf;
        view_dispatcher_send_custom_event(
            app->view_dispatcher, Proxmark5CustomEventOpenClassicDumpPage);
        break;
    default:
        break;
    }
}

static void fmps_cxt_request_nfc_name_iso(void* context) {
    Proxmark5App* app = context;
    app->nfc_save_mode = NfcSaveModeIso14443a;
    view_dispatcher_send_custom_event(
        app->view_dispatcher, Proxmark5CustomEventOpenNfcNameInput);
}

static void fmps_cxt_request_nfc_name_classic(void* context) {
    Proxmark5App* app = context;
    app->nfc_save_mode = NfcSaveModeClassicDump;
    view_dispatcher_send_custom_event(
        app->view_dispatcher, Proxmark5CustomEventOpenNfcNameInput);
}

static void fmps_cxt_classic_back(void* context) {
    Proxmark5App* app = context;
    view_dispatcher_switch_to_view(app->view_dispatcher, OperateHfViewId);
}

static void fmps_cxt_nfc_name_input_callback(void* context) {
    Proxmark5App* app = context;
    if(app->nfc_save_mode == NfcSaveModeClassicDump) {
        classic_dump_page_save_as(app->classic_dump_page, app->nfc_name);
        view_dispatcher_switch_to_view(app->view_dispatcher, ClassicDumpPageViewId);
    } else {
        simple_cmd_page_save_as(app->simple_cmd_page, app->nfc_name);
        view_dispatcher_switch_to_view(app->view_dispatcher, SimpleCmdPageViewId);
    }
}

static bool fmps_cxt_custom_event_callback(void* context, uint32_t event) {
    Proxmark5App* app = context;

    switch(event) {
    case Proxmark5CustomEventOpenOperatePage:
        view_dispatcher_switch_to_view(app->view_dispatcher, OperateRootViewId);
        return true;
    case Proxmark5CustomEventOpenToolsMenu:
        view_dispatcher_switch_to_view(app->view_dispatcher, OperateToolsViewId);
        return true;
    case Proxmark5CustomEventOpenHfMenu:
        view_dispatcher_switch_to_view(app->view_dispatcher, OperateHfViewId);
        return true;
    case Proxmark5CustomEventOpenSimpleCmdPage:
        view_dispatcher_switch_to_view(app->view_dispatcher, SimpleCmdPageViewId);
        simple_cmd_page_start(app->simple_cmd_page, app->pending_simple_cmd);
        return true;
    case Proxmark5CustomEventOpenClassicDumpPage:
        classic_dump_page_reset(app->classic_dump_page);
        view_dispatcher_switch_to_view(app->view_dispatcher, ClassicDumpPageViewId);
        return true;
    case Proxmark5CustomEventOpenNfcNameInput: {
        const char* def = (app->nfc_save_mode == NfcSaveModeClassicDump) ?
                              classic_dump_page_default_name(app->classic_dump_page) :
                              simple_cmd_page_default_name(app->simple_cmd_page);
        snprintf(app->nfc_name, sizeof(app->nfc_name), "%s", def);
        text_input_reset(app->text_input);
        text_input_set_header_text(app->text_input, "Save as (.nfc)");
        text_input_set_result_callback(
            app->text_input,
            fmps_cxt_nfc_name_input_callback,
            app,
            app->nfc_name,
            sizeof(app->nfc_name),
            true);
        text_input_set_minimum_length(app->text_input, 1);
        view_dispatcher_switch_to_view(app->view_dispatcher, NfcNameInputViewId);
        return true;
    }
    default:
        return false;
    }
}

static uint32_t operate_root_previous_callback(void* context) {
    UNUSED(context);
    return MainPageViewId;
}

static uint32_t operate_group_previous_callback(void* context) {
    UNUSED(context);
    return OperateRootViewId;
}

static uint32_t simple_cmd_page_previous_callback(void* context) {
    Proxmark5App* app = context;
    simple_cmd_page_stop(app->simple_cmd_page);
    return (app->leaf_back == LeafBackToTools) ? OperateToolsViewId : OperateHfViewId;
}

static uint32_t classic_dump_page_previous_callback(void* context) {
    Proxmark5App* app = context;
    classic_dump_page_stop(app->classic_dump_page);
    return OperateHfViewId;
}

static uint32_t nfc_name_input_previous_callback(void* context) {
    UNUSED(context);
    Proxmark5App* app = s_app;
    if(!app) {
        return VIEW_NONE;
    }
    if(app->nfc_save_mode == NfcSaveModeClassicDump) {
        return ClassicDumpPageViewId;
    }
    return SimpleCmdPageViewId;
}

Proxmark5App* proxmark5_app_alloc() {
    Proxmark5App* app = calloc(1, sizeof(Proxmark5App));
    s_app = app;
    app->gui = furi_record_open(RECORD_GUI);
    app->leaf_back = LeafBackToHf;

    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    app->main_page = main_page_create(fmps_cxt_open_operate_page, app);
    view_dispatcher_add_view(app->view_dispatcher, MainPageViewId, app->main_page->canvas_view);

    app->operate_page =
        operate_page_create(fmps_cxt_operate_item, app, fmps_cxt_navigate_group, app);
    view_set_previous_callback(
        operate_page_get_root_view(app->operate_page), operate_root_previous_callback);
    view_set_previous_callback(
        operate_page_get_tools_view(app->operate_page), operate_group_previous_callback);
    view_set_previous_callback(
        operate_page_get_hf_view(app->operate_page), operate_group_previous_callback);
    view_dispatcher_add_view(
        app->view_dispatcher, OperateRootViewId, operate_page_get_root_view(app->operate_page));
    view_dispatcher_add_view(
        app->view_dispatcher, OperateToolsViewId, operate_page_get_tools_view(app->operate_page));
    view_dispatcher_add_view(
        app->view_dispatcher, OperateHfViewId, operate_page_get_hf_view(app->operate_page));

    app->simple_cmd_page = simple_cmd_page_create();
    simple_cmd_page_set_save_request_callback(
        app->simple_cmd_page, fmps_cxt_request_nfc_name_iso, app);
    view_set_previous_callback(
        simple_cmd_page_get_view(app->simple_cmd_page), simple_cmd_page_previous_callback);
    view_set_context(simple_cmd_page_get_view(app->simple_cmd_page), app);
    view_dispatcher_add_view(
        app->view_dispatcher, SimpleCmdPageViewId, simple_cmd_page_get_view(app->simple_cmd_page));

    app->classic_dump_page = classic_dump_page_create();
    classic_dump_page_set_save_request_callback(
        app->classic_dump_page, fmps_cxt_request_nfc_name_classic, app);
    classic_dump_page_set_back_request_callback(
        app->classic_dump_page, fmps_cxt_classic_back, app);
    classic_dump_page_set_mfkey_request_callback(
        app->classic_dump_page, fmps_cxt_launch_mfkey, app);
    view_set_previous_callback(
        classic_dump_page_get_view(app->classic_dump_page), classic_dump_page_previous_callback);
    view_set_context(classic_dump_page_get_view(app->classic_dump_page), app);
    view_dispatcher_add_view(
        app->view_dispatcher,
        ClassicDumpPageViewId,
        classic_dump_page_get_view(app->classic_dump_page));

    app->text_input = text_input_alloc();
    view_set_previous_callback(
        text_input_get_view(app->text_input), nfc_name_input_previous_callback);
    view_dispatcher_add_view(
        app->view_dispatcher, NfcNameInputViewId, text_input_get_view(app->text_input));

    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, fmps_cxt_back_event_callback);
    view_dispatcher_set_custom_event_callback(
        app->view_dispatcher, fmps_cxt_custom_event_callback);
    return app;
}

void proxmark5_app_free(Proxmark5App* app) {
    furi_assert(app);
    if(s_app == app) {
        s_app = NULL;
    }

    view_dispatcher_remove_view(app->view_dispatcher, MainPageViewId);
    view_dispatcher_remove_view(app->view_dispatcher, OperateRootViewId);
    view_dispatcher_remove_view(app->view_dispatcher, OperateToolsViewId);
    view_dispatcher_remove_view(app->view_dispatcher, OperateHfViewId);
    view_dispatcher_remove_view(app->view_dispatcher, SimpleCmdPageViewId);
    view_dispatcher_remove_view(app->view_dispatcher, ClassicDumpPageViewId);
    view_dispatcher_remove_view(app->view_dispatcher, NfcNameInputViewId);
    if(app->main_page) {
        main_page_free(app->main_page);
        app->main_page = NULL;
    }
    if(app->operate_page) {
        operate_page_free(app->operate_page);
        app->operate_page = NULL;
    }
    if(app->simple_cmd_page) {
        simple_cmd_page_free(app->simple_cmd_page);
        app->simple_cmd_page = NULL;
    }
    if(app->classic_dump_page) {
        classic_dump_page_free(app->classic_dump_page);
        app->classic_dump_page = NULL;
    }
    if(app->text_input) {
        text_input_free(app->text_input);
        app->text_input = NULL;
    }
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);
    free(app);
}

int32_t fmps_cxt_app(void* p) {
    UNUSED(p);

    furi_hal_power_insomnia_enter();
    proxmark5_com_init();

    Proxmark5App* app = proxmark5_app_alloc();
    view_dispatcher_switch_to_view(app->view_dispatcher, MainPageViewId);
    view_dispatcher_run(app->view_dispatcher);
    proxmark5_app_free(app);

    proxmark5_com_deinit();
    furi_hal_power_insomnia_exit();

    return 0;
}
