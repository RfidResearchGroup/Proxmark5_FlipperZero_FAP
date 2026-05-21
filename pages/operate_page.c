#include <furi.h>
#include "operate_page.h"

typedef enum {
    OperateSubmenuIndexReadHitag2 = 0,
} OperateSubmenuIndex;

static void operate_page_submenu_callback(void* context, uint32_t index) {
    UNUSED(context);

    switch(index) {
    case OperateSubmenuIndexReadHitag2:
        FURI_LOG_I("OperatePage", "ReadHitag2 selected");
        break;
    default:
        break;
    }
}

OperatePage* operate_page_create(void) {
    OperatePage* operate_page = calloc(1, sizeof(OperatePage));
    if(!operate_page) {
        return NULL;
    }

    operate_page->submenu = submenu_alloc();
    submenu_set_header(operate_page->submenu, "Functions Menu");
    submenu_add_item(
        operate_page->submenu,
        "ReadHitag2",
        OperateSubmenuIndexReadHitag2,
        operate_page_submenu_callback,
        operate_page);

    return operate_page;
}

void operate_page_free(OperatePage* operate_page) {
    if(!operate_page) {
        return;
    }

    if(operate_page->submenu) {
        submenu_free(operate_page->submenu);
        operate_page->submenu = NULL;
    }

    free(operate_page);
}
