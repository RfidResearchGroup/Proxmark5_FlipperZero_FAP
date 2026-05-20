#ifndef FMPS_CXT_CANVAS_VIEW_H
#define FMPS_CXT_CANVAS_VIEW_H

#include <gui/view.h>
#include <gui/modules/text_box.h>
#include <gui/modules/variable_item_list.h>

typedef struct {
    // The canvas view for the main page
    View* canvas_view;

    // Proxmark5 handshake thread
    FuriThread* proxmark5_handshake_thread;
    bool proxmark5_handshake_thread_running;
} MainPage;

MainPage* main_page_create(void);
void main_page_free(MainPage* main_page);

#endif // FMPS_CXT_CANVAS_VIEW_H
