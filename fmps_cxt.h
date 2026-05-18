#ifndef FMPS_CXT_H
#define FMPS_CXT_H

#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <gui/modules/text_box.h>
#include <gui/modules/variable_item_list.h>

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    SceneManager* scene_manager;

    TextBox* text_box;

    FuriThread* proxmark5_com_thread;
    bool proxmark5_com_thread_running;
} Proxmark5App;

#endif // FMPS_CXT_H
