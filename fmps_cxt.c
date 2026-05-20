#include <furi.h>
#include <furi_hal.h>
#include <fmps_cxt_icons.h>
#include "fmps_cxt.h"
#include "proxmark5_com.h"

// For GUI:
//  https://github.com/jamisonderek/flipper-zero-tutorials/wiki/User-Interface#viewdisptacher

// The callback for the back event, it will stop the view dispatcher which will exit the app
static bool fmps_cxt_back_event_callback(void* context) {
    Proxmark5App* app = context;
    view_dispatcher_stop(app->view_dispatcher);
    return true;
}

Proxmark5App* proxmark5_app_alloc() {
    Proxmark5App* app = calloc(1, sizeof(Proxmark5App));
    app->gui = furi_record_open(RECORD_GUI);

    // Allocate the view dispatcher and attach it to the GUI
    app->view_dispatcher = view_dispatcher_alloc();
    // Attach the view dispatcher to the GUI with fullscreen type
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    // Create and add the main page view to the view dispatcher
    app->main_page = main_page_create();
    view_dispatcher_add_view(app->view_dispatcher, MainPageViewId, app->main_page->canvas_view);

    // Set the back event callback to handle the back button press, and pass the app context to it
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, fmps_cxt_back_event_callback);
    return app;
}

void proxmark5_app_free(Proxmark5App* app) {
    furi_assert(app);

    view_dispatcher_remove_view(app->view_dispatcher, MainPageViewId);
    if(app->main_page) {
        main_page_free(app->main_page);
        app->main_page = NULL;
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
