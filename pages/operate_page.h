#ifndef OPERATE_PAGE_H
#define OPERATE_PAGE_H

#include <gui/modules/submenu.h>

typedef void (*OperatePageReadHitag2Callback)(void* context);
typedef void (*OperatePageOpenStatusTestCallback)(void* context);

typedef struct {
    Submenu* submenu;

    OperatePageReadHitag2Callback open_read_hitag2_callback;
    void* open_read_hitag2_callback_context;

    // Each opens the shared status_page with a different title/fetch pair -
    // see fmps_cxt.c's custom_event_callback for which fetch function each
    // one runs.
    OperatePageOpenStatusTestCallback open_capabilities_callback;
    void* open_capabilities_callback_context;
    OperatePageOpenStatusTestCallback open_ping_callback;
    void* open_ping_callback_context;
    OperatePageOpenStatusTestCallback open_flash_chip_callback;
    void* open_flash_chip_callback_context;
    OperatePageOpenStatusTestCallback open_battery_callback;
    void* open_battery_callback_context;
} OperatePage;

OperatePage* operate_page_create(
    OperatePageReadHitag2Callback open_read_hitag2_callback,
    void* open_read_hitag2_callback_context,
    OperatePageOpenStatusTestCallback open_capabilities_callback,
    void* open_capabilities_callback_context,
    OperatePageOpenStatusTestCallback open_ping_callback,
    void* open_ping_callback_context,
    OperatePageOpenStatusTestCallback open_flash_chip_callback,
    void* open_flash_chip_callback_context,
    OperatePageOpenStatusTestCallback open_battery_callback,
    void* open_battery_callback_context);
void operate_page_free(OperatePage* operate_page);

#endif // OPERATE_PAGE_H
