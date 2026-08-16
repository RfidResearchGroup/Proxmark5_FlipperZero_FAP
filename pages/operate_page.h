#ifndef OPERATE_PAGE_H
#define OPERATE_PAGE_H

#include <gui/modules/submenu.h>
#include <gui/view.h>

typedef void (*OperatePageItemCallback)(void* context, uint32_t index);

/** Leaf actions (sent to the app). */
typedef enum {
    OperateItemPing = 0,
    OperateItemVersion,
    OperateItemHf14a,
    OperateItemAutopwn,
} OperateItemIndex;

/** Root menu entries (open a group). */
typedef enum {
    OperateRootTools = 0,
    OperateRootHf,
} OperateRootIndex;

typedef void (*OperatePageNavigateCallback)(void* context, OperateRootIndex group);

typedef struct {
    Submenu* root;
    Submenu* tools;
    Submenu* hf;
    OperatePageItemCallback item_callback;
    void* item_callback_context;
    OperatePageNavigateCallback navigate_callback;
    void* navigate_callback_context;
} OperatePage;

OperatePage* operate_page_create(
    OperatePageItemCallback item_callback,
    void* item_callback_context,
    OperatePageNavigateCallback navigate_callback,
    void* navigate_callback_context);
void operate_page_free(OperatePage* operate_page);

View* operate_page_get_root_view(OperatePage* operate_page);
View* operate_page_get_tools_view(OperatePage* operate_page);
View* operate_page_get_hf_view(OperatePage* operate_page);

#endif // OPERATE_PAGE_H
