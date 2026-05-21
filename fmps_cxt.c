#include <furi.h>
#include <furi_hal.h>
#include <fmps_cxt_icons.h>
#include "fmps_cxt.h"
#include "proxmark5_com.h"

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
    view_dispatcher_send_custom_event(app->view_dispatcher, Proxmark5CustomEventOpenOperatePage);
}

static bool fmps_cxt_custom_event_callback(void* context, uint32_t event) {
    Proxmark5App* app = context;

    switch(event) {
    case Proxmark5CustomEventOpenOperatePage:
        view_dispatcher_switch_to_view(app->view_dispatcher, OperatePageViewId);
        return true;
    default:
        return false;
    }
}

static uint32_t operate_page_previous_callback(void* context) {
    UNUSED(context);
    return MainPageViewId;
}

Proxmark5App* proxmark5_app_alloc() {
    Proxmark5App* app = calloc(1, sizeof(Proxmark5App));
    app->gui = furi_record_open(RECORD_GUI);

    // Allocate the view dispatcher and attach it to the GUI
    app->view_dispatcher = view_dispatcher_alloc();
    // Attach the view dispatcher to the GUI with fullscreen type
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    // Create and add the main page view to the view dispatcher
    app->main_page = main_page_create(fmps_cxt_open_operate_page, app);
    view_dispatcher_add_view(app->view_dispatcher, MainPageViewId, app->main_page->canvas_view);

    // Create and add the operate submenu page to the view dispatcher
    app->operate_page = operate_page_create();
    view_set_previous_callback(
        submenu_get_view(app->operate_page->submenu), operate_page_previous_callback);
    view_dispatcher_add_view(
        app->view_dispatcher, OperatePageViewId, submenu_get_view(app->operate_page->submenu));

    // Set the back event callback to handle the back button press, and pass the app context to it
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, fmps_cxt_back_event_callback);
    view_dispatcher_set_custom_event_callback(
        app->view_dispatcher, fmps_cxt_custom_event_callback);
    return app;
}

void proxmark5_app_free(Proxmark5App* app) {
    furi_assert(app);

    view_dispatcher_remove_view(app->view_dispatcher, MainPageViewId);
    view_dispatcher_remove_view(app->view_dispatcher, OperatePageViewId);
    if(app->main_page) {
        main_page_free(app->main_page);
        app->main_page = NULL;
    }
    if(app->operate_page) {
        operate_page_free(app->operate_page);
        app->operate_page = NULL;
    }
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
    view_dispatcher_run(app->view_dispatcher);
    // Free the app context and exit
    proxmark5_app_free(app);

    // Deinitialize proxmark5 communication interfaces
    proxmark5_com_deinit();

    return 0;
}
