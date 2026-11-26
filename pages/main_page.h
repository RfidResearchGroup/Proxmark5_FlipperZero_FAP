#ifndef FMPS_CXT_CANVAS_VIEW_H
#define FMPS_CXT_CANVAS_VIEW_H

#include <stdatomic.h>
#include <gui/view.h>

typedef enum {
    MainPageRead,
    MainPageHardware,
    MainPageChoiceCount,
} MainPageChoice;

typedef void (*MainPageSelectCallback)(void* context, MainPageChoice choice);

typedef struct {
    // The canvas view for the main page
    View* canvas_view;

    MainPageSelectCallback on_select;
    void* callback_context;

    // Proxmark5 handshake thread
    FuriThread* proxmark5_handshake_thread;
    atomic_bool proxmark5_handshake_thread_running;
} MainPage;

MainPage* main_page_create(MainPageSelectCallback on_select, void* context);
void main_page_start(MainPage* page);
void main_page_stop_worker(MainPage* page);
void main_page_free(MainPage* main_page);

#endif // FMPS_CXT_CANVAS_VIEW_H
