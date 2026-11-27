#include <furi.h>
#include <string.h>
#include "status_page_tests.h"
#include "proxmark5_frame.h"
#include "pm3_cmd.h"

static int status_test_send_and_wait(
    uint16_t cmd,
    uint8_t* payload,
    size_t payload_len,
    PacketResponseNG* resp,
    uint32_t timeout_ms,
    volatile bool* cancel_requested) {
    if(cancel_requested && *cancel_requested) return PM3_EOPABORTED;
    if(!SendCommandNG(cmd, payload, payload_len)) return PM3_EIO;
    return proxmark5_frame_wait(cmd, resp, timeout_ms, cancel_requested);
}

static int status_test_quick_request(
    uint16_t cmd, PacketResponseNG* resp, volatile bool* cancel_requested) {
    static const uint32_t timeouts_ms[] = {250, 500, 500};
    clearCommandBuffer();
    for(size_t i = 0; i < COUNT_OF(timeouts_ms); i++) {
        int result = status_test_send_and_wait(
            cmd, NULL, 0, resp, timeouts_ms[i], cancel_requested);
        if(result != PM3_ETIMEOUT) return result;
    }
    return PM3_ETIMEOUT;
}

static int status_test_radio_request(
    uint16_t cmd,
    uint8_t action,
    PacketResponseNG* resp,
    uint32_t timeout_ms,
    volatile bool* cancel_requested) {
    // The PM5 can still be querying the BWM while it wakes.
    clearCommandBuffer();
    return status_test_send_and_wait(
        cmd, &action, sizeof(action), resp, timeout_ms, cancel_requested);
}

int status_test_capabilities_fetch(
    volatile bool* cancel_requested,
    char lines[STATUS_PAGE_MAX_LINES][STATUS_PAGE_LINE_LEN],
    int* out_line_count,
    bool* bwm_compiled) {
    *out_line_count = 0;

    PacketResponseNG resp;
    int wait_result = status_test_quick_request(CMD_CAPABILITIES, &resp, cancel_requested);
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
    *bwm_compiled = caps.compiled_with_bwm;

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
    snprintf(lines[n++], STATUS_PAGE_LINE_LEN, "Link:   CEP / SPI");
    *out_line_count = n;
    return PM3_SUCCESS;
}

bool status_test_ping_fetch(uint32_t* rtt_ms) {
    return proxmark5_frame_echo(rtt_ms);
}

void status_test_chip_id_fetch(
    volatile bool* cancel_requested,
    char row[STATUS_PAGE_LINE_LEN]) {
    PacketResponseNG resp;
    if(status_test_quick_request(CMD_MAIN_CHIP_UNIQUEID, &resp, cancel_requested) !=
           PM3_SUCCESS ||
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

int status_test_format_battery(
    const bwm_battery_info_t* info,
    char lines[STATUS_PAGE_MAX_LINES][STATUS_PAGE_LINE_LEN]) {
    if(!info) {
        snprintf(lines[0], STATUS_PAGE_LINE_LEN, "Battery unavailable");
        return 1;
    }

    if(!info->bwm_present) {
        snprintf(lines[0], STATUS_PAGE_LINE_LEN, "N/A (no BWM fitted)");
        return 1;
    }
    if(!info->gauge_ok) {
        snprintf(lines[0], STATUS_PAGE_LINE_LEN, "Gauge not responding");
        return 1;
    }

    static const char* const charge_states[] = {"idle", "precharge", "charging", "full"};
    int temp = info->temp_c10 < 0 ? -info->temp_c10 : info->temp_c10;
    snprintf(lines[0], STATUS_PAGE_LINE_LEN, "%u%%  %s", info->soc_pct,
             info->charge_status < COUNT_OF(charge_states) ? charge_states[info->charge_status] :
                                                           "unknown");
    snprintf(lines[1], STATUS_PAGE_LINE_LEN, "Voltage: %u mV", info->voltage_mv);
    snprintf(lines[2], STATUS_PAGE_LINE_LEN, "Current: %+d mA", info->current_ma);
    snprintf(lines[3], STATUS_PAGE_LINE_LEN, "Temp: %s%d.%d C",
             info->temp_c10 < 0 ? "-" : "", temp / 10, temp % 10);
    snprintf(lines[4], STATUS_PAGE_LINE_LEN, "Remain: %u mAh", info->remaining_mah);
    snprintf(lines[5], STATUS_PAGE_LINE_LEN, "Full: %u mAh", info->full_charge_mah);
    snprintf(lines[6], STATUS_PAGE_LINE_LEN, "Design: %u mAh", info->design_cap_mah);
    if(info->charger_fault) {
        snprintf(lines[7], STATUS_PAGE_LINE_LEN, "Charger fault: 0x%02X", info->charger_fault);
    } else if(info->full_charge_mah) {
        snprintf(lines[7], STATUS_PAGE_LINE_LEN, "Health: %u%%", info->health_pct);
    } else {
        snprintf(lines[7], STATUS_PAGE_LINE_LEN, "Health unavailable");
    }
    return 8;
}

int status_test_wifi_fetch(
    volatile bool* cancel_requested,
    char rows[STATUS_PAGE_ROWS_PER_PAGE][STATUS_PAGE_LINE_LEN]) {
    PacketResponseNG resp;
    int result = status_test_radio_request(
        CMD_PM5_BWM_WIFI, BWM_WIFI_ACTION_STATUS, &resp, 4500, cancel_requested);

    const char* state = "No reply";
    if(result == PM3_EIO) {
        state = "Send failed";
    } else if(result == PM3_SUCCESS) {
        result = resp.status;
        if(resp.status == PM3_ENOTIMPL) {
            state = "Unavailable";
        } else if(resp.status != PM3_SUCCESS) {
            state = "BWM no reply";
        } else if(resp.length != 5) {
            state = "Invalid reply";
            result = PM3_ESOFT;
        } else {
            const uint8_t* data = resp.data.asBytes;
            switch(data[0]) {
            case BWM_WIFI_STATE_DISCONNECTED:
                state = "Disconnected";
                break;
            case BWM_WIFI_STATE_CONNECTING:
                state = "Connecting";
                break;
            case BWM_WIFI_STATE_CONNECTED:
                state = "Connected";
                if(data[1] || data[2] || data[3] || data[4]) {
                    snprintf(rows[1], STATUS_PAGE_LINE_LEN, "IP: %u.%u.%u.%u",
                             data[1], data[2], data[3], data[4]);
                } else {
                    snprintf(rows[1], STATUS_PAGE_LINE_LEN, "IP unavailable");
                }
                break;
            case BWM_WIFI_STATE_RECONNECT:
                state = "Reconnecting";
                break;
            case BWM_WIFI_STATE_OFF:
            case BWM_WIFI_STATE_STOPPED:
                state = "Off";
                break;
            default:
                state = "Unknown";
                break;
            }
        }
    }
    snprintf(rows[0], STATUS_PAGE_LINE_LEN, "%s", state);
    return result;
}

void status_test_ble_fetch(
    volatile bool* cancel_requested,
    char rows[STATUS_PAGE_ROWS_PER_PAGE][STATUS_PAGE_LINE_LEN]) {
    PacketResponseNG reply;
    int result = status_test_radio_request(
        CMD_PM5_BWM_BLE, BWM_BLE_ACTION_STATUS, &reply, 8000, cancel_requested);
    if(result != PM3_SUCCESS) {
        snprintf(rows[0], STATUS_PAGE_LINE_LEN, "%s",
                 result == PM3_EIO ? "Send failed" : "No reply");
        return;
    }
    if(reply.status != PM3_SUCCESS || reply.length < sizeof(bwm_ble_status_t)) {
        const char* message = "BWM no reply";
        if(reply.status == PM3_ENOTIMPL) {
            message = "Unavailable";
        } else if(reply.status == PM3_SUCCESS) {
            message = "Invalid reply";
        }
        snprintf(rows[0], STATUS_PAGE_LINE_LEN, "%s", message);
        return;
    }

    const bwm_ble_status_t* status = (const bwm_ble_status_t*)reply.data.asBytes;
    static const char* const states[] = {"Off", "Advertising", "Connected"};
    const char* state = "Unknown";
    if(status->enabled == 0) {
        state = "Off";
    } else if(status->state == 0 && status->enabled == 1) {
        state = "On, inactive";
    } else if(status->state < COUNT_OF(states)) {
        state = states[status->state];
    }
    snprintf(rows[0], STATUS_PAGE_LINE_LEN, "BLE: %s", state);
    if(status->name[0]) {
        snprintf(rows[1], STATUS_PAGE_LINE_LEN, "Name: %.*s",
                 (int)sizeof(status->name), status->name);
    } else {
        snprintf(rows[1], STATUS_PAGE_LINE_LEN, "Name unavailable");
    }

    const char* pairing = "Unknown";
    if(status->bonding == 0) {
        pairing = status->enabled == 1 ? "Off (open)" : "Off";
    } else if(status->bonding == 1) {
        pairing = "Required";
    }
    snprintf(rows[2], STATUS_PAGE_LINE_LEN, "Pairing: %s", pairing);
    if(status->bonded_count <= BWM_BLE_BONDED_MAX) {
        snprintf(rows[3], STATUS_PAGE_LINE_LEN, "Bonded devices: %u%s", status->bonded_count,
                 status->bonded_count == BWM_BLE_BONDED_MAX ? "+" : "");
    } else {
        snprintf(rows[3], STATUS_PAGE_LINE_LEN, "Bonded devices: Unknown");
    }
}

int status_test_cep_fetch(
    volatile bool* cancel_requested,
    char rows[STATUS_PAGE_ROWS_PER_PAGE][STATUS_PAGE_LINE_LEN],
    bwm_battery_info_t* battery) {
    PacketResponseNG resp;
    int result = status_test_quick_request(CMD_CEP_STATUS, &resp, cancel_requested);
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
