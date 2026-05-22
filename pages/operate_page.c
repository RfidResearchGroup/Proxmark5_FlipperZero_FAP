#include <furi.h>
#include "operate_page.h"
#include "proxmark5_frame.h"
#include "pm3_cmd.h"
#include "hitag.h"

typedef enum {
    OperateSubmenuIndexReadHitag2 = 0,
} OperateSubmenuIndex;

// TODO DXL !!!!!!!!!!!!!! FOR TEST ONLY, REMOVE THIS LATER !!!!!!!!!!!!!!

static int test(void) {
    lf_hitag_data_t packet;
    memset(&packet, 0, sizeof(packet));

    uint8_t key[6];
    memcpy(key, "MIKR", 4);

    int pm3cmd = CMD_LF_HITAG_READER;
    packet.cmd = HT2F_PASSWORD;
    memcpy(packet.pwd, key, sizeof(packet.pwd));

    clearCommandBuffer();
    SendCommandNG(pm3cmd, (uint8_t*)&packet, sizeof(packet));

    PacketResponseNG resp;
    if(WaitForResponseTimeout(pm3cmd, &resp, 2000) == false) {
        FURI_LOG_W("OperatePage", "Wait for Hitag2 response timeout");
        SendCommandNG(CMD_BREAK_LOOP, NULL, 0);
        return PM3_ETIMEOUT;
    }

    if(resp.status != PM3_SUCCESS) {
        FURI_LOG_W("OperatePage", "Hitag2 operation failed");
        return PM3_ESOFT;
    }

    return PM3_SUCCESS;
}

// ----------------------------------------------------------------------

static void operate_page_submenu_callback(void* context, uint32_t index) {
    UNUSED(context);

    switch(index) {
    case OperateSubmenuIndexReadHitag2:
        test();
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
