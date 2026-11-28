#ifndef OPERATE_PAGE_H
#define OPERATE_PAGE_H

#include <gui/modules/submenu.h>

typedef enum {
    OperateOpenMenu,
    OperateOpenHardware,
    OperateOpenSaved,
    OperateBrowseEm4100,
    OperateBrowseNtag213,
    OperateNtag21xActions,
    OperateSaveNtag21x,
    OperateWriteNtag21x,
    OperateWriteNtag21xUrl,
    OperateWriteNtag21xTextAndUrl,
    OperateWriteNtag21xSmartPoster,
    OperateWriteNtag21xVCard,
    OperateConfirmNtag21x,
    OperateFinishNtag21x,
    OperateImportNtag213,
    OperateBrowseTagReads,
    OperateReadHitag2,
    OperateReadHf14a,
    OperateOpenStatus,
    OperateOpenAntennaCheck,
    OperateOpenLiveVoltage,
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
