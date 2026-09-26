#include <furi.h>
#include "operate_page.h"

typedef enum {
    OperateSubmenuIndexReadHitag2 = 0,
    OperateSubmenuIndexCapabilities,
    OperateSubmenuIndexPing,
    OperateSubmenuIndexFlashChipId,
    OperateSubmenuIndexBattery,
} OperateSubmenuIndex;

static void operate_page_submenu_callback(void* context, uint32_t index) {
    OperatePage* operate_page = context;
    if(!operate_page) {
        return;
    }

    switch(index) {
    case OperateSubmenuIndexReadHitag2:
        if(operate_page->open_read_hitag2_callback) {
            operate_page->open_read_hitag2_callback(
                operate_page->open_read_hitag2_callback_context);
        }
        break;
    case OperateSubmenuIndexCapabilities:
        if(operate_page->open_capabilities_callback) {
            operate_page->open_capabilities_callback(
                operate_page->open_capabilities_callback_context);
        }
        break;
    case OperateSubmenuIndexPing:
        if(operate_page->open_ping_callback) {
            operate_page->open_ping_callback(operate_page->open_ping_callback_context);
        }
        break;
    case OperateSubmenuIndexFlashChipId:
        if(operate_page->open_flash_chip_callback) {
            operate_page->open_flash_chip_callback(
                operate_page->open_flash_chip_callback_context);
        }
        break;
    case OperateSubmenuIndexBattery:
        if(operate_page->open_battery_callback) {
            operate_page->open_battery_callback(operate_page->open_battery_callback_context);
        }
        break;
    default:
        break;
    }
}

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
    void* open_battery_callback_context) {
    OperatePage* operate_page = calloc(1, sizeof(OperatePage));
    if(!operate_page) {
        return NULL;
    }

    operate_page->open_read_hitag2_callback = open_read_hitag2_callback;
    operate_page->open_read_hitag2_callback_context = open_read_hitag2_callback_context;
    operate_page->open_capabilities_callback = open_capabilities_callback;
    operate_page->open_capabilities_callback_context = open_capabilities_callback_context;
    operate_page->open_ping_callback = open_ping_callback;
    operate_page->open_ping_callback_context = open_ping_callback_context;
    operate_page->open_flash_chip_callback = open_flash_chip_callback;
    operate_page->open_flash_chip_callback_context = open_flash_chip_callback_context;
    operate_page->open_battery_callback = open_battery_callback;
    operate_page->open_battery_callback_context = open_battery_callback_context;

    operate_page->submenu = submenu_alloc();
    submenu_set_header(operate_page->submenu, "Functions Menu");
    submenu_add_item(
        operate_page->submenu,
        "ReadHitag2",
        OperateSubmenuIndexReadHitag2,
        operate_page_submenu_callback,
        operate_page);
    submenu_add_item(
        operate_page->submenu,
        "Capabilities",
        OperateSubmenuIndexCapabilities,
        operate_page_submenu_callback,
        operate_page);
    submenu_add_item(
        operate_page->submenu,
        "Ping Test",
        OperateSubmenuIndexPing,
        operate_page_submenu_callback,
        operate_page);
    submenu_add_item(
        operate_page->submenu,
        "Flash/Chip ID",
        OperateSubmenuIndexFlashChipId,
        operate_page_submenu_callback,
        operate_page);
    submenu_add_item(
        operate_page->submenu,
        "Battery",
        OperateSubmenuIndexBattery,
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
