#include <furi.h>
#include <string.h>
#include "status_page_tests.h"
#include "proxmark5_frame.h"
#include "pm3_cmd.h"

// Back off between retries of read-only commands. Keep the full reply
// window after the last send so a late reply can still arrive.
#define STATUS_TEST_RETRY_AFTER_MS 250U
#define STATUS_TEST_QUICK_TIMEOUT_MS 500
// Back-to-back status requests can drop replies on CEP.
#define STATUS_TEST_REQUEST_GAP_MS 50

static int status_test_send_and_wait(
    uint16_t cmd,
    uint8_t* payload,
    size_t payload_len,
    PacketResponseNG* resp,
    uint32_t timeout_ms,
    volatile bool* cancel_requested) {
    // Long BWM requests can still be working when the retry is sent.
    int attempts = timeout_ms == STATUS_TEST_QUICK_TIMEOUT_MS ? 3 : 2;
    for(int attempt = 0; attempt < attempts; attempt++) {
        furi_delay_ms(STATUS_TEST_REQUEST_GAP_MS);
        if(cancel_requested && *cancel_requested) {
            return PM3_EOPABORTED;
        }
        if(attempt == 0) clearCommandBuffer();
        if(!SendCommandNG(cmd, payload, payload_len)) return PM3_EIO;
        uint32_t wait_ms =
            attempt + 1 < attempts ? (STATUS_TEST_RETRY_AFTER_MS << attempt) : timeout_ms;
        int result = proxmark5_frame_wait(cmd, resp, wait_ms, cancel_requested);
        if(result != PM3_ETIMEOUT) return result;
    }
    return PM3_ETIMEOUT;
}

int status_test_capabilities_fetch(
    volatile bool* cancel_requested,
    char lines[STATUS_PAGE_MAX_LINES][STATUS_PAGE_LINE_LEN],
    int* out_line_count) {
    *out_line_count = 0;

    PacketResponseNG resp;
    int wait_result = status_test_send_and_wait(
        CMD_CAPABILITIES, NULL, 0, &resp, STATUS_TEST_QUICK_TIMEOUT_MS, cancel_requested);
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
    snprintf(lines[n++], STATUS_PAGE_LINE_LEN, "PM5:   %s", caps.is_pm5 ? "yes" : "no");
    snprintf(
        lines[n++],
        STATUS_PAGE_LINE_LEN,
        "BWM:   %s   CEP:   %s",
        caps.compiled_with_bwm ? "y" : "n",
        caps.compiled_with_cep ? "y" : "n");
    snprintf(
        lines[n++],
        STATUS_PAGE_LINE_LEN,
        "Flash:   %s   SC:   %s",
        caps.hw_available_flash ? "y" : "n",
        caps.hw_available_smartcard ? "y" : "n");
    snprintf(lines[n++], STATUS_PAGE_LINE_LEN, "BigBuf:   %lu", (unsigned long)caps.bigbuf_size);
    snprintf(lines[n++], STATUS_PAGE_LINE_LEN, "MaxCmdData:   %u", caps.max_cmd_data_size);
    snprintf(lines[n++], STATUS_PAGE_LINE_LEN, "Baud:   %lu", (unsigned long)caps.baudrate);
    *out_line_count = n;
    return PM3_SUCCESS;
}

bool status_test_ping_fetch(uint32_t* rtt_ms) {
    furi_delay_ms(STATUS_TEST_REQUEST_GAP_MS);
    return proxmark5_frame_echo(rtt_ms);
}

void status_test_chip_id_fetch(
    volatile bool* cancel_requested,
    char row[STATUS_PAGE_LINE_LEN]) {
    PacketResponseNG resp;
    if(status_test_send_and_wait(
           CMD_MAIN_CHIP_UNIQUEID, NULL, 0, &resp, STATUS_TEST_QUICK_TIMEOUT_MS,
           cancel_requested) != PM3_SUCCESS ||
       resp.status != PM3_SUCCESS || resp.length == 0) {
        snprintf(row, STATUS_PAGE_LINE_LEN, "ChipID: N/A");
        return;
    }

    char hex[24] = {0};
    size_t bytes = MIN(resp.length, (sizeof(hex) - 1) / 2);
    for(size_t i = 0; i < bytes; i++) {
        snprintf(hex + 2 * i, 3, "%02X", resp.data.asBytes[i]);
    }
    snprintf(row, STATUS_PAGE_LINE_LEN, "ChipID:   %s", hex);
}

int status_test_battery_fetch(
    volatile bool* cancel_requested,
    const bwm_battery_info_t* snapshot,
    char lines[STATUS_PAGE_MAX_LINES][STATUS_PAGE_LINE_LEN],
    int* out_line_count) {
    *out_line_count = 0;

    bwm_battery_info_t info;
    if(snapshot) {
        info = *snapshot;
    } else {
        PacketResponseNG resp;
        int wait_result = status_test_send_and_wait(
            CMD_PM5_BWM_GET_BATTERY, NULL, 0, &resp, 2000, cancel_requested);
        if(wait_result != PM3_SUCCESS) {
            snprintf(lines[0], STATUS_PAGE_LINE_LEN, "N/A (no reply)");
            *out_line_count = 1;
            return wait_result;
        }
        if(resp.status != PM3_SUCCESS || resp.length < sizeof(info)) {
            snprintf(lines[0], STATUS_PAGE_LINE_LEN, "N/A (bad reply)");
            *out_line_count = 1;
            return PM3_ESOFT;
        }
        memcpy(&info, resp.data.asBytes, sizeof(info));
    }

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
        "SoC:   %u%%   %s",
        info.soc_pct,
        chg_status_str[info.charge_status & 0x03]);
    snprintf(
        lines[n++],
        STATUS_PAGE_LINE_LEN,
        "V:   %umV   I:   %dmA",
        info.voltage_mv,
        info.current_ma);
    snprintf(lines[n++], STATUS_PAGE_LINE_LEN, "Remain:   %umAh", info.remaining_mah);
    snprintf(lines[n++], STATUS_PAGE_LINE_LEN, "Temp:   %d.%dC", info.temp_c10 / 10, tabs % 10);
    if(info.full_charge_mah > 0 && n < STATUS_PAGE_MAX_LINES) {
        snprintf(lines[n++], STATUS_PAGE_LINE_LEN, "Health:   %u%%", info.health_pct);
    }
    if(info.charger_fault != 0 && n < STATUS_PAGE_MAX_LINES) {
        snprintf(lines[n++], STATUS_PAGE_LINE_LEN, "Fault:   0x%02X", info.charger_fault);
    }
    *out_line_count = n;
    return PM3_SUCCESS;
}

int status_test_cep_fetch(
    volatile bool* cancel_requested,
    char rows[STATUS_PAGE_ROWS_PER_PAGE][STATUS_PAGE_LINE_LEN],
    bwm_battery_info_t* battery) {
    PacketResponseNG resp;
    int result = status_test_send_and_wait(
        CMD_CEP_STATUS, NULL, 0, &resp, STATUS_TEST_QUICK_TIMEOUT_MS, cancel_requested);
    if(result != PM3_SUCCESS) {
        snprintf(rows[0], STATUS_PAGE_LINE_LEN, "N/A (no reply)");
        return result;
    }
    if(resp.status != PM3_SUCCESS || resp.length < sizeof(cep_status_t)) {
        snprintf(rows[0], STATUS_PAGE_LINE_LEN, "N/A (bad reply)");
        return PM3_ESOFT;
    }

    cep_status_t status;
    memcpy(&status, resp.data.asBytes, sizeof(status));
    *battery = status.battery;
    snprintf(rows[0], STATUS_PAGE_LINE_LEN, "CEP active:   %s", status.cep_active ? "Y" : "N");
    snprintf(rows[1], STATUS_PAGE_LINE_LEN, "FW:   %.*s",
             (int)sizeof(status.fw_version), status.fw_version);
    return PM3_SUCCESS;
}
