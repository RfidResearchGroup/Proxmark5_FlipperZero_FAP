#ifndef OPERATE_PAGE_H
#define OPERATE_PAGE_H

#include <gui/modules/submenu.h>

typedef enum {
    OperateOpenMenu,
    OperateOpenHardware,
    OperateReadHitag2,
    OperateReadHf14a,
    OperateOpenStatus,
    OperateOpenAntennaCheck,
    OperateOpenLiveVoltage,
} OperateAction;

typedef struct {
    Submenu* submenu;
    Submenu* hardware_submenu;
} OperatePage;

OperatePage* operate_page_create(SubmenuItemCallback callback, void* context);
void operate_page_free(OperatePage* operate_page);

#endif // OPERATE_PAGE_H
