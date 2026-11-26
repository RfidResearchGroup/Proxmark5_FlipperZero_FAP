#include <furi.h>
#include "operate_page.h"
#include "../readers/simple_readers.h"

OperatePage* operate_page_create(SubmenuItemCallback callback, void* context) {
    OperatePage* operate_page = malloc(sizeof(OperatePage));
    operate_page->submenu = submenu_alloc();
    Submenu* menu = operate_page->submenu;
    submenu_set_header(menu, "Read");
    submenu_add_item(menu, "HF 14a Reader", OperateReadHf14a, callback, context);
    submenu_add_item(menu, "Hitag2 Reader", OperateReadHitag2, callback, context);
    for(size_t i = 0; i < simple_reader_count; i++) {
        submenu_add_item(menu, simple_readers[i]->title, OperateSimpleReaderFirst + i, callback, context);
    }

    operate_page->saved_submenu = submenu_alloc();
    submenu_set_header(operate_page->saved_submenu, "Saved");
    submenu_add_item(operate_page->saved_submenu, "EM4100 Tags", OperateBrowseEm4100, callback, context);
    submenu_add_item(operate_page->saved_submenu, "Tag Reads", OperateBrowseTagReads, callback, context);

    operate_page->hardware_submenu = submenu_alloc();
    submenu_set_header(operate_page->hardware_submenu, "Hardware");
    submenu_add_item(operate_page->hardware_submenu, "Hardware Status", OperateOpenStatus, callback, context);
    submenu_add_item(operate_page->hardware_submenu, "Antenna Check", OperateOpenAntennaCheck, callback, context);
    submenu_add_item(operate_page->hardware_submenu, "PM5 Report", OperateOpenReport, callback, context);
    return operate_page;
}

void operate_page_free(OperatePage* operate_page) {
    submenu_free(operate_page->saved_submenu);
    submenu_free(operate_page->hardware_submenu);
    submenu_free(operate_page->submenu);
    free(operate_page);
}
