#include <furi.h>
#include <string.h>
#include "status_page_tests.h"
#include "proxmark5_frame.h"
#include "pm3_cmd.h"

// Shared cancel-aware wait, same pattern as
// read_hitag2_wait_for_response_interruptible() in read_hitag2_page.c -
// deliberately not just calling the bare WaitForResponseTimeout() so every
// test here stays cancellable via the page's Back button.
static int status_test_wait(
    uint16_t cmd,
    PacketResponseNG* resp,
    uint32_t timeout_ms,
    volatile bool* cancel_requested) {
    uint32_t start_time = furi_get_tick();

    while((furi_get_tick() - start_time) < timeout_ms) {
        if(cancel_requested && *cancel_requested) {
            return PM3_EOPABORTED;
        }
        if(proxmark5_frame_take_by_cmd(cmd, resp)) {
            return PM3_SUCCESS;
        }
        furi_delay_ms(10);
    }

    if(cancel_requested && *cancel_requested) {
        return PM3_EOPABORTED;
    }
    return proxmark5_frame_take_by_cmd(cmd, resp) ? PM3_SUCCESS : PM3_ETIMEOUT;
}

int status_test_capabilities_fetch(
    volatile bool* cancel_requested,
    char lines[STATUS_PAGE_MAX_LINES][STATUS_PAGE_LINE_LEN],
    int* out_line_count) {
    *out_line_count = 0;

    clearCommandBuffer();
    if(cancel_requested && *cancel_requested) {
        return PM3_EOPABORTED;
    }
    SendCommandNG(CMD_CAPABILITIES, NULL, 0);

    PacketResponseNG resp;
    int wait_result = status_test_wait(CMD_CAPABILITIES, &resp, 2000, cancel_requested);
    if(wait_result != PM3_SUCCESS) {
        return wait_result;
    }

    // Same validation the desktop client does before trusting the struct -
    // refuse to parse on a size/version mismatch rather than misreading it.
    if(resp.status != PM3_SUCCESS || resp.length != sizeof(capabilities_t) ||
       resp.data.asBytes[0] != CAPABILITIES_VERSION) {
        snprintf(lines[0], STATUS_PAGE_LINE_LEN, "Bad reply (ver mismatch)");
        *out_line_count = 1;
        return PM3_EDEVNOTSUPP;
    }

    capabilities_t caps;
    memcpy(&caps, resp.data.asBytes, sizeof(caps));

    int n = 0;
    snprintf(lines[n++], STATUS_PAGE_LINE_LEN, "PM5: %s", caps.is_pm5 ? "yes" : "no");
    snprintf(
        lines[n++],
        STATUS_PAGE_LINE_LEN,
        "BWM:%s CEP:%s",
        caps.compiled_with_bwm ? "y" : "n",
        caps.compiled_with_cep ? "y" : "n");
    snprintf(
        lines[n++],
        STATUS_PAGE_LINE_LEN,
        "Flash:%s SC:%s",
        caps.hw_available_flash ? "y" : "n",
        caps.hw_available_smartcard ? "y" : "n");
    snprintf(lines[n++], STATUS_PAGE_LINE_LEN, "BigBuf: %lu", (unsigned long)caps.bigbuf_size);
    snprintf(lines[n++], STATUS_PAGE_LINE_LEN, "MaxCmdData: %u", caps.max_cmd_data_size);
    snprintf(lines[n++], STATUS_PAGE_LINE_LEN, "Baud: %lu", (unsigned long)caps.baudrate);
    *out_line_count = n;
    return PM3_SUCCESS;
}

int status_test_ping_fetch(
    volatile bool* cancel_requested,
    char lines[STATUS_PAGE_MAX_LINES][STATUS_PAGE_LINE_LEN],
    int* out_line_count) {
    *out_line_count = 0;

    uint8_t payload[8];
    for(size_t i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(i * 0x11 + 1); // arbitrary distinct pattern
    }

    clearCommandBuffer();
    if(cancel_requested && *cancel_requested) {
        return PM3_EOPABORTED;
    }

    uint32_t start = furi_get_tick();
    SendCommandNG(CMD_PING, payload, sizeof(payload));

    PacketResponseNG resp;
    int wait_result = status_test_wait(CMD_PING, &resp, 2000, cancel_requested);
    uint32_t rtt_ms = furi_get_tick() - start;
    if(wait_result != PM3_SUCCESS) {
        return wait_result;
    }

    bool match = (resp.status == PM3_SUCCESS) && (resp.length == sizeof(payload)) &&
                 (memcmp(resp.data.asBytes, payload, sizeof(payload)) == 0);

    int n = 0;
    snprintf(lines[n++], STATUS_PAGE_LINE_LEN, "Sent: %u bytes", (unsigned)sizeof(payload));
    snprintf(lines[n++], STATUS_PAGE_LINE_LEN, "Echo: %s", match ? "OK" : "MISMATCH");
    snprintf(lines[n++], STATUS_PAGE_LINE_LEN, "RTT: %lums", (unsigned long)rtt_ms);
    *out_line_count = n;
    return match ? PM3_SUCCESS : PM3_ESOFT;
}

int status_test_flash_chip_fetch(
    volatile bool* cancel_requested,
    char lines[STATUS_PAGE_MAX_LINES][STATUS_PAGE_LINE_LEN],
    int* out_line_count) {
    int n = 0;
    PacketResponseNG resp;

    clearCommandBuffer();
    if(cancel_requested && *cancel_requested) {
        *out_line_count = n;
        return PM3_EOPABORTED;
    }
    SendCommandNG(CMD_FLASHMEM_GET_INFO, NULL, 0);
    if(status_test_wait(CMD_FLASHMEM_GET_INFO, &resp, 1500, cancel_requested) == PM3_SUCCESS &&
       resp.status == PM3_SUCCESS && resp.length >= sizeof(spi_flash_t)) {
        spi_flash_t info;
        memcpy(&info, resp.data.asBytes, sizeof(info));
        snprintf(
            lines[n++],
            STATUS_PAGE_LINE_LEN,
            "Flash %02X/%02X J:%04X",
            info.manufacturer_id,
            info.device_id,
            info.jedec_id);
        snprintf(lines[n++], STATUS_PAGE_LINE_LEN, "Size: %u*64K", info.pages64k);
    } else {
        snprintf(lines[n++], STATUS_PAGE_LINE_LEN, "Flash: N/A");
    }

    clearCommandBuffer();
    if(cancel_requested && *cancel_requested) {
        *out_line_count = n;
        return PM3_EOPABORTED;
    }
    SendCommandNG(CMD_FLASHMEM_GET_ID, NULL, 0);
    if(status_test_wait(CMD_FLASHMEM_GET_ID, &resp, 1500, cancel_requested) == PM3_SUCCESS &&
       resp.status == PM3_SUCCESS && resp.length >= sizeof(uint64_t) &&
       n < STATUS_PAGE_MAX_LINES) {
        uint64_t uid = 0;
        memcpy(&uid, resp.data.asBytes, sizeof(uid));
        snprintf(
            lines[n++],
            STATUS_PAGE_LINE_LEN,
            "FlashUID:%08lX%08lX",
            (unsigned long)(uid >> 32),
            (unsigned long)(uid & 0xFFFFFFFFu));
    }

    clearCommandBuffer();
    if(cancel_requested && *cancel_requested) {
        *out_line_count = n;
        return PM3_EOPABORTED;
    }
    SendCommandNG(CMD_MAIN_CHIP_UNIQUEID, NULL, 0);
    if(status_test_wait(CMD_MAIN_CHIP_UNIQUEID, &resp, 1500, cancel_requested) == PM3_SUCCESS &&
       resp.status == PM3_SUCCESS && resp.length > 0 && n < STATUS_PAGE_MAX_LINES) {
        char hex[24] = {0};
        int hn = 0;
        for(int i = 0; i < resp.length && hn < (int)sizeof(hex) - 3; i++) {
            hn += snprintf(hex + hn, sizeof(hex) - hn, "%02X", resp.data.asBytes[i]);
        }
        snprintf(lines[n++], STATUS_PAGE_LINE_LEN, "ChipID:%s", hex);
    } else if(n < STATUS_PAGE_MAX_LINES) {
        snprintf(lines[n++], STATUS_PAGE_LINE_LEN, "ChipID: N/A");
    }

    *out_line_count = n;
    return PM3_SUCCESS; // partial results still count as a completed test run
}

int status_test_battery_fetch(
    volatile bool* cancel_requested,
    char lines[STATUS_PAGE_MAX_LINES][STATUS_PAGE_LINE_LEN],
    int* out_line_count) {
    *out_line_count = 0;

    clearCommandBuffer();
    if(cancel_requested && *cancel_requested) {
        return PM3_EOPABORTED;
    }
    SendCommandNG(CMD_PM5_BWM_GET_BATTERY, NULL, 0);

    PacketResponseNG resp;
    int wait_result = status_test_wait(CMD_PM5_BWM_GET_BATTERY, &resp, 2000, cancel_requested);
    if(wait_result != PM3_SUCCESS) {
        // Most likely an older PM5 build without this command at all.
        snprintf(lines[0], STATUS_PAGE_LINE_LEN, "N/A (no reply)");
        *out_line_count = 1;
        return wait_result;
    }

    if(resp.status != PM3_SUCCESS || resp.length < sizeof(bwm_battery_info_t)) {
        snprintf(lines[0], STATUS_PAGE_LINE_LEN, "N/A (bad reply)");
        *out_line_count = 1;
        return PM3_ESOFT;
    }

    bwm_battery_info_t info;
    memcpy(&info, resp.data.asBytes, sizeof(info));

    if(!info.bwm_present) {
        snprintf(lines[0], STATUS_PAGE_LINE_LEN, "N/A (no BWM fitted)");
        *out_line_count = 1;
        return PM3_SUCCESS;
    }
    if(!info.gauge_ok) {
        snprintf(lines[0], STATUS_PAGE_LINE_LEN, "Gauge not responding");
        *out_line_count = 1;
        return PM3_SUCCESS;
    }

    static const char* const chg_status_str[] = {"idle", "pre-chg", "charging", "done"};
    int tabs = info.temp_c10 < 0 ? -info.temp_c10 : info.temp_c10;

    int n = 0;
    snprintf(
        lines[n++],
        STATUS_PAGE_LINE_LEN,
        "SoC:%u%% %s",
        info.soc_pct,
        chg_status_str[info.charge_status & 0x03]);
    snprintf(
        lines[n++], STATUS_PAGE_LINE_LEN, "V:%umV I:%dmA", info.voltage_mv, info.current_ma);
    snprintf(lines[n++], STATUS_PAGE_LINE_LEN, "Remain: %umAh", info.remaining_mah);
    snprintf(lines[n++], STATUS_PAGE_LINE_LEN, "Temp: %d.%dC", info.temp_c10 / 10, tabs % 10);
    if(info.full_charge_mah > 0 && n < STATUS_PAGE_MAX_LINES) {
        snprintf(lines[n++], STATUS_PAGE_LINE_LEN, "Health: %u%%", info.health_pct);
    }
    if(info.charger_fault != 0 && n < STATUS_PAGE_MAX_LINES) {
        snprintf(lines[n++], STATUS_PAGE_LINE_LEN, "Fault: 0x%02X", info.charger_fault);
    }
    *out_line_count = n;
    return PM3_SUCCESS;
}
