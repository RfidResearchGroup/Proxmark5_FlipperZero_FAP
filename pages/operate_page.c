#include <furi.h>
#include "operate_page.h"

static void operate_root_callback(void* context, uint32_t index) {
    OperatePage* page = context;
    if(!page || !page->navigate_callback) {
        return;
    }
    page->navigate_callback(page->navigate_callback_context, (OperateRootIndex)index);
}

static void operate_tools_callback(void* context, uint32_t index) {
    OperatePage* page = context;
    if(!page || !page->item_callback) {
        return;
    }
    if(index == 0) {
        page->item_callback(page->item_callback_context, OperateItemPing);
    } else if(index == 1) {
        page->item_callback(page->item_callback_context, OperateItemVersion);
    }
}

static void operate_hf_callback(void* context, uint32_t index) {
    OperatePage* page = context;
    if(!page || !page->item_callback) {
        return;
    }
    if(index == 0) {
        page->item_callback(page->item_callback_context, OperateItemHf14a);
    } else if(index == 1) {
        page->item_callback(page->item_callback_context, OperateItemAutopwn);
    }
}

OperatePage* operate_page_create(
    OperatePageItemCallback item_callback,
    void* item_callback_context,
    OperatePageNavigateCallback navigate_callback,
    void* navigate_callback_context) {
    OperatePage* page = calloc(1, sizeof(OperatePage));
    if(!page) {
        return NULL;
    }

    page->item_callback = item_callback;
    page->item_callback_context = item_callback_context;
    page->navigate_callback = navigate_callback;
    page->navigate_callback_context = navigate_callback_context;

    page->root = submenu_alloc();
    submenu_set_header(page->root, "Proxmark5");
    submenu_add_item(page->root, "PM5 Tools", OperateRootTools, operate_root_callback, page);
    submenu_add_item(page->root, "HF RFID", OperateRootHf, operate_root_callback, page);

    page->tools = submenu_alloc();
    submenu_set_header(page->tools, "PM5 Tools");
    submenu_add_item(page->tools, "Ping", 0, operate_tools_callback, page);
    submenu_add_item(page->tools, "HW Version", 1, operate_tools_callback, page);

    page->hf = submenu_alloc();
    submenu_set_header(page->hf, "HF RFID");
    submenu_add_item(page->hf, "14a Reader", 0, operate_hf_callback, page);
    submenu_add_item(page->hf, "MFC Autopwn", 1, operate_hf_callback, page);

    return page;
}

void operate_page_free(OperatePage* page) {
    if(!page) {
        return;
    }
    if(page->root) {
        submenu_free(page->root);
        page->root = NULL;
    }
    if(page->tools) {
        submenu_free(page->tools);
        page->tools = NULL;
    }
    if(page->hf) {
        submenu_free(page->hf);
        page->hf = NULL;
    }
    free(page);
}

View* operate_page_get_root_view(OperatePage* page) {
    return page ? submenu_get_view(page->root) : NULL;
}

View* operate_page_get_tools_view(OperatePage* page) {
    return page ? submenu_get_view(page->tools) : NULL;
}

View* operate_page_get_hf_view(OperatePage* page) {
    return page ? submenu_get_view(page->hf) : NULL;
}
