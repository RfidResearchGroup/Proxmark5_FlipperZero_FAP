#include <furi.h>
#include <gui/canvas.h>
#include "main_page.h"
#include "proxmark5_com.h"
#include "proxmark5_frame.h"

typedef struct {
    MainPageChoice selected;
    bool connected;
} MainPageModel;

static const char* const main_page_items[MainPageChoiceCount] = {
    [MainPageRead] = "Read",
    [MainPageSaved] = "Saved",
    [MainPageHardware] = "Hardware",
};

static bool main_page_input_callback(InputEvent* event, void* context) {
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return false;
    MainPage* page = context;
    if(event->key == InputKeyUp || event->key == InputKeyDown) {
        MainPageModel* model = view_get_model(page->canvas_view);
        unsigned step = event->key == InputKeyDown ? 1U : MainPageChoiceCount - 1U;
        model->selected = (model->selected + step) % MainPageChoiceCount;
        view_commit_model(page->canvas_view, true);
        return true;
    }
    if(event->type != InputTypeShort || event->key != InputKeyOk) return false;

    MainPageModel* model = view_get_model(page->canvas_view);
    MainPageChoice choice = model->selected;
    bool available = model->connected || choice == MainPageSaved;
    view_commit_model(page->canvas_view, false);
    if(available) page->on_select(page->callback_context, choice);
    return true;
}

// The thread for proxmark5 communication
static int32_t proxmark5_handshake_task(void* context) {
    MainPage* page = context;

    while(1) {
        // If the thread is marked as not running, exit the loop
        if(!atomic_load_explicit(&page->proxmark5_handshake_thread_running, memory_order_relaxed)) {
            break;
        }

        // Try to perform the proxmark5 handshake, if successful, show the success message and exit the loop
        proxmark5_frame_begin_operation(NULL);
        bool connected = proxmark5_com_handshake();
        proxmark5_frame_end_operation();
        if(connected) {
            // Switch to the success state and update the view to show the success message
            MainPageModel* model = view_get_model(page->canvas_view);
            model->connected = true;
            view_commit_model(page->canvas_view, true);
            FURI_LOG_I("proxmark5_handshake_task", "Proxmark5 handshake successful");
            break;
        }
        // Sleep for a while before the next handshake attempt
        furi_delay_ms(100);
    }
    // Mark the thread as not running when exiting the loop
    atomic_store_explicit(&page->proxmark5_handshake_thread_running, false, memory_order_relaxed);
    return 0;
}

static void main_page_draw(Canvas* canvas, void* context) {
    const MainPageModel* model = context;
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 4, 11, "Proxmark5");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(
        canvas, 124, 11, AlignRight, AlignBottom, model->connected ? "Connected" : "Connecting");
    canvas_draw_line(canvas, 0, 14, 127, 14);

    for(unsigned i = 0; i < MainPageChoiceCount; i++) {
        uint8_t top = 16 + i * 12;
        if(i == model->selected) {
            canvas_draw_rbox(canvas, 1, top, 126, 11, 2);
            canvas_set_color(canvas, ColorWhite);
        }
        canvas_draw_str(canvas, 8, top + 8, main_page_items[i]);
        canvas_set_color(canvas, ColorBlack);
    }
}

// Join before removing the view. Join unconditionally because
// the running flag can go false before the kernel marks the thread Stopped,
// so trusting it risks freeing a thread the kernel still considers running.
void main_page_stop_worker(MainPage* page) {
    if(page && page->proxmark5_handshake_thread) {
        atomic_store_explicit(&page->proxmark5_handshake_thread_running, false, memory_order_relaxed);
        furi_thread_join(page->proxmark5_handshake_thread);
        furi_thread_free(page->proxmark5_handshake_thread);
        page->proxmark5_handshake_thread = NULL;
    }
}

// Start the proxmark5 handshake thread
void main_page_start(MainPage* page) {
    if(page->proxmark5_handshake_thread) {
        return;
    }
    // Allocate and start the proxmark5 handshake thread
    // Handshake path may call deep PM3 driver code; keep a safer stack budget on F0.
    page->proxmark5_handshake_thread =
        furi_thread_alloc_ex("HandshakeTask", 1024, proxmark5_handshake_task, page);
    atomic_store_explicit(&page->proxmark5_handshake_thread_running, true, memory_order_relaxed);
    furi_thread_start(page->proxmark5_handshake_thread);
}

/**
 * @brief Create the main page, which includes a canvas view
 * 
 * @return MainPage* The created main page
 */
MainPage* main_page_create(MainPageSelectCallback on_select, void* context) {
    MainPage* main_page = calloc(1, sizeof(MainPage));
    if(!main_page) {
        return NULL;
    }
    atomic_init(&main_page->proxmark5_handshake_thread_running, false);
    // Allocate the canvas view for the main page
    main_page->canvas_view = view_alloc();
    main_page->on_select = on_select;
    main_page->callback_context = context;

    // Set the context for the canvas view callbacks(Working on the like 'view_set_input_callback')
    view_set_context(main_page->canvas_view, main_page);

    view_allocate_model(main_page->canvas_view, ViewModelTypeLocking, sizeof(MainPageModel));
    view_set_draw_callback(main_page->canvas_view, main_page_draw);
    view_set_input_callback(main_page->canvas_view, main_page_input_callback);

    return main_page;
}

// Free the main page and its resources
void main_page_free(MainPage* main_page) {
    if(main_page) {
        main_page_stop_worker(main_page);
        if(main_page->canvas_view) {
            view_free_model(main_page->canvas_view);
            view_free(main_page->canvas_view);
        }
        free(main_page);
    }
}
