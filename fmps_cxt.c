#include <furi.h>
#include <furi_hal.h>
#include <fmps_cxt_icons.h>
#include "fmps_cxt.h"
#include "proxmark5_com.h"

// The callback for the back event, it will stop the view dispatcher which will exit the app
static bool fmps_cxt_back_event_callback(void* context) {
    Proxmark5App* app = context;
    view_dispatcher_stop(app->view_dispatcher);

    // Signal the communication thread to stop and wait for it to finish
    app->proxmark5_com_thread_running = false;
    furi_thread_join(app->proxmark5_com_thread);

    return true;
}

// The thread for proxmark5 communication
static int32_t proxmark5_com_task(void* context) {
    Proxmark5App* app = context;
    while(1) {
        if(!app->proxmark5_com_thread_running) {
            break;
        }
        // Perform the handshake with proxmark5, and update the text box with the result
        char* msg;
        if(proxmark5_com_handshake()) {
            msg = "Proxmark5 handshake \n  -> successful!";
        } else {
            msg = "Proxmark5 handshake \n  -> failed!";
        }
        text_box_set_text(app->text_box, msg);
        // Sleep for a while before the next handshake attempt
        furi_delay_ms(500);
    }
    return 0;
}

Proxmark5App* proxmark5_app_alloc() {
    Proxmark5App* app = malloc(sizeof(Proxmark5App));
    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    app->text_box = text_box_alloc();
    view_dispatcher_add_view(app->view_dispatcher, 0, text_box_get_view(app->text_box));

    // Set the back event callback to handle the back button press, and pass the app context to it
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, fmps_cxt_back_event_callback);
    return app;
}

void proxmark5_app_free(Proxmark5App* app) {
    furi_assert(app);
    view_dispatcher_remove_view(app->view_dispatcher, 0);
    text_box_free(app->text_box);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);
    furi_thread_free(app->proxmark5_com_thread);
    free(app);
}

// Start the proxmark5 communication thread
void proxmark5_com_task_start(Proxmark5App* app) {
    app->proxmark5_com_thread =
        furi_thread_alloc_ex("Proxmark5ComThread", 1024, proxmark5_com_task, app);
    app->proxmark5_com_thread_running = true;
    furi_thread_start(app->proxmark5_com_thread);
}

// Furi application entry point
int32_t fmps_cxt_app(void* p) {
    UNUSED(p);

    // First initialize the communication with proxmark5, this will set up the GPIO pin and SPI bus
    proxmark5_com_init();

    Proxmark5App* app = proxmark5_app_alloc();
    text_box_set_text(app->text_box, "Hello World 11");
    view_dispatcher_switch_to_view(app->view_dispatcher, 0);
    proxmark5_com_task_start(app);
    view_dispatcher_run(app->view_dispatcher);
    proxmark5_app_free(app);

    // Deinitialize the communication with proxmark5, this will reset the GPIO pin and SPI bus
    proxmark5_com_deinit();

    return 0;
}
