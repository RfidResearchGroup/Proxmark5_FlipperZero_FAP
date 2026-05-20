#include <furi.h>
#include <fmps_cxt_icons.h>
#include <gui/canvas.h>
#include "main_page.h"
#include "proxmark5_com.h"

typedef enum {
    MainPageHandshakeRunning,
    MainPageShowDeviceInfo,
} MainPageState;

typedef struct {
    // The main page struct pointer
    MainPage* page;
    // The state of the main page, it can be used to determine what to display on the canvas
    MainPageState state;
} MainPageModel;

// Draw the handshake successful message on the canvas
void draw_handshake_ok_message(Canvas* canvas) {
    canvas_clear(canvas);
    // Show the iceman logo with a success message
    canvas_draw_icon(canvas, 3, 3, &I_iceman_logo);
    // Draw the success message
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 45, 15, "Proxmark5");
    canvas_draw_str(canvas, 45, 30, "(Iceman Edition)");
}

// Draw the handshake running message on the canvas
void draw_handshake_running(Canvas* canvas) {
    // Draw the icons
    canvas_draw_icon(canvas, 3, 3, &I_connect_me);
    canvas_draw_icon(canvas, 88, 4, &I_iceman_logo);
    // Draw the line between the icons
    canvas_draw_line(canvas, 64, 19, 75, 19);
    // Draw the line end polygon (arrow)
    canvas_draw_line(canvas, 77, 15, 83, 19);
    canvas_draw_line(canvas, 83, 19, 77, 23);
    canvas_draw_line(canvas, 77, 23, 77, 19);
    canvas_draw_line(canvas, 77, 19, 77, 15);
    // Draw the text
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 9, 55, "Proxmark5 connecting");
}

// The thread for proxmark5 communication
static int32_t proxmark5_handshake_task(void* context) {
    MainPage* page = context;
    while(1) {
        // If the thread is marked as not running, exit the loop
        if(!page->proxmark5_handshake_thread_running) {
            break;
        }
        // Try to perform the proxmark5 handshake, if successful, show the success message and exit the loop
        if(proxmark5_com_handshake()) {
            // Switch to the success state and update the view to show the success message
            MainPageModel* model = view_get_model(page->canvas_view);
            model->state = MainPageShowDeviceInfo;
            view_commit_model(page->canvas_view, true);
            FURI_LOG_I("proxmark5_handshake_task", "Proxmark5 handshake successful");
            break;
        }
        // Sleep for a while before the next handshake attempt
        furi_delay_ms(100);
    }
    // Mark the thread as not running when exiting the loop
    page->proxmark5_handshake_thread_running = false;
    return 0;
}

/**
 * @brief Canvas draw callback for the main page
 * 
 * @param canvas The canvas to draw on
 * @param ctx Context
 */
static void fmps_cxt_canvas_draw_callback(Canvas* canvas, void* ctx) {
    if(ctx == NULL) {
        return;
    }

    MainPageModel* model = ctx;
    switch(model->state) {
    case MainPageHandshakeRunning:
        draw_handshake_running(canvas);
        break;
    case MainPageShowDeviceInfo:
        draw_handshake_ok_message(canvas);
        break;
    default:
        break;
    }
}

// Stop the proxmark5 handshake thread
static void proxmark5_handshake_task_stop(MainPage* page) {
    // If the thread is running, stop it and free the resources
    if(page->proxmark5_handshake_thread) {
        if(page->proxmark5_handshake_thread_running) {
            page->proxmark5_handshake_thread_running = false;
            furi_thread_join(page->proxmark5_handshake_thread);
        }
        furi_thread_free(page->proxmark5_handshake_thread);
        page->proxmark5_handshake_thread = NULL;
    }
}

// Start the proxmark5 handshake thread
void proxmark5_handshake_task_start(MainPage* page) {
    if(page->proxmark5_handshake_thread) {
        return;
    }
    // Allocate and start the proxmark5 handshake thread
    page->proxmark5_handshake_thread =
        furi_thread_alloc_ex("HandshakeTask", 1024, proxmark5_handshake_task, page);
    page->proxmark5_handshake_thread_running = true;
    furi_thread_start(page->proxmark5_handshake_thread);
}

/**
 * @brief Create the main page, which includes a canvas view
 * 
 * @return MainPage* The created main page
 */
MainPage* main_page_create(void) {
    MainPage* main_page = calloc(1, sizeof(MainPage));
    if(!main_page) {
        return NULL;
    }
    // Allocate the canvas view for the main page
    main_page->canvas_view = view_alloc();

    // Set the context for the canvas view callbacks(Working on the like 'view_set_input_callback')
    view_set_context(main_page->canvas_view, main_page);

    // The model as a pointer to the main page struct,
    // so we can access the state and other info in the draw callback
    view_allocate_model(main_page->canvas_view, ViewModelTypeLockFree, sizeof(MainPageModel));
    MainPageModel* model = view_get_model(main_page->canvas_view);
    model->page = main_page;
    model->state = MainPageHandshakeRunning; // Default to the handshake running state

    // Set the draw callback for the canvas view
    view_set_draw_callback(main_page->canvas_view, fmps_cxt_canvas_draw_callback);

    // Start the proxmark5 handshake thread
    proxmark5_handshake_task_start(main_page);
    return main_page;
}

// Free the main page and its resources
void main_page_free(MainPage* main_page) {
    if(main_page) {
        proxmark5_handshake_task_stop(main_page);
        if(main_page->canvas_view) {
            view_set_context(main_page->canvas_view, NULL);
            view_free_model(main_page->canvas_view);
            view_free(main_page->canvas_view);
        }
        free(main_page);
    }
}
