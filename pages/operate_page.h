#ifndef OPERATE_PAGE_H
#define OPERATE_PAGE_H

#include <gui/modules/submenu.h>

typedef enum {
    OperateOpenMenu,
    OperateOpenHardware,
    OperateOpenSaved,
    OperateBrowseEm4100,
    OperateBrowseTagReads,
    OperateReadHitag2,
    OperateReadHf14a,
    OperateOpenStatus,
    OperateOpenAntennaCheck,
    OperateOpenLiveVoltage,
    OperateOpenReport,
    OperateSimpleReaderFirst,
} OperateAction;

typedef struct {
    Submenu* submenu;
    Submenu* hardware_submenu;
    Submenu* saved_submenu;
} OperatePage;

OperatePage* operate_page_create(SubmenuItemCallback callback, void* context);
void operate_page_free(OperatePage* operate_page);

#endif // OPERATE_PAGE_H
