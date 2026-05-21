#ifndef FMPS_CXT_H
#define FMPS_CXT_H

#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>

// Pages
#include "pages/main_page.h"
#include "pages/operate_page.h"

typedef enum {
    // The main page of the app, showing the connection status and some basic info
    MainPageViewId,
    // The operate page, showing operation submenu
    OperatePageViewId,
} Proxmark5ViewId;

typedef enum {
    Proxmark5CustomEventOpenOperatePage,
} Proxmark5CustomEvent;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;

    // Pages
    MainPage* main_page;
    OperatePage* operate_page;
} Proxmark5App;

#endif // FMPS_CXT_H
