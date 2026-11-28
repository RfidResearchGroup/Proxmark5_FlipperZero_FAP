#ifndef FMPS_CXT_H
#define FMPS_CXT_H

#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <gui/modules/text_box.h>

// Pages
#include "pages/main_page.h"
#include "pages/operate_page.h"
#include "pages/read_hitag2_page.h"
#include "pages/read_hf14a_page.h"
#include "pages/status_page.h"
#include "pages/antenna_check_page.h"
#include "pages/live_voltage_page.h"
#include "pages/simple_reader_page.h"
#include "pages/status_report.h"

typedef enum {
    // The main page of the app, showing the connection status and some basic info
    MainPageViewId,
    // The operate page, showing operation submenu
    OperatePageViewId,
    // Read Hitag2 operation page
    ReadHitag2PageViewId,
    // HF 14a Reader operation page
    ReadHf14aPageViewId,
    StatusPageViewId,
    AntennaCheckPageViewId,
    LiveVoltagePageViewId,
    HardwareMenuViewId,
    SavedMenuViewId,
    SavedTagReadViewId,
    SimpleReaderPageViewId,
    StatusReportViewId,
} Proxmark5ViewId;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;

    // Pages
    MainPage* main_page;
    OperatePage* operate_page;
    ReadHitag2Page* read_hitag2_page;
    ReadHf14aPage* read_hf14a_page;
    StatusPage* status_page;
    AntennaCheckPage* antenna_check_page;
    LiveVoltagePage* live_voltage_page;
    SimpleReaderPage* simple_reader_page;
    StatusReport* status_report;
    TextBox* saved_tag_view;
    FuriString* saved_tag_text;
} Proxmark5App;

#endif // FMPS_CXT_H
