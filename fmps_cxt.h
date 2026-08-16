#ifndef FMPS_CXT_H
#define FMPS_CXT_H

#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <gui/modules/text_input.h>

#include "pages/main_page.h"
#include "pages/operate_page.h"
#include "pages/simple_cmd_page.h"
#include "pages/classic_dump_page.h"

#define NFC_NAME_SIZE 48

typedef enum {
    MainPageViewId,
    OperateRootViewId,
    OperateToolsViewId,
    OperateHfViewId,
    SimpleCmdPageViewId,
    ClassicDumpPageViewId,
    NfcNameInputViewId,
} Proxmark5ViewId;

typedef enum {
    Proxmark5CustomEventOpenOperatePage,
    Proxmark5CustomEventOpenToolsMenu,
    Proxmark5CustomEventOpenHfMenu,
    Proxmark5CustomEventOpenSimpleCmdPage,
    Proxmark5CustomEventOpenClassicDumpPage,
    Proxmark5CustomEventOpenNfcNameInput,
} Proxmark5CustomEvent;

typedef enum {
    NfcSaveModeIso14443a = 0,
    NfcSaveModeClassicDump,
} NfcSaveMode;

/** Where Back returns from a simple/autopwn leaf. */
typedef enum {
    LeafBackToTools = 0,
    LeafBackToHf,
} LeafBackTarget;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;

    MainPage* main_page;
    OperatePage* operate_page;
    SimpleCmdPage* simple_cmd_page;
    ClassicDumpPage* classic_dump_page;
    SimpleCmdKind pending_simple_cmd;
    LeafBackTarget leaf_back;

    TextInput* text_input;
    char nfc_name[NFC_NAME_SIZE];
    NfcSaveMode nfc_save_mode;
} Proxmark5App;

#endif // FMPS_CXT_H
