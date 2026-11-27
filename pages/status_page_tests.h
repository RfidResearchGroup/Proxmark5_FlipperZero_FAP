#ifndef STATUS_PAGE_TESTS_H
#define STATUS_PAGE_TESTS_H

#include "status_page.h"
#include "pm3_cmd.h"

int status_test_capabilities_fetch(
    volatile bool* cancel_requested,
    char lines[STATUS_PAGE_MAX_LINES][STATUS_PAGE_LINE_LEN],
    int* out_line_count,
    bool* bwm_compiled);

bool status_test_ping_fetch(uint32_t* rtt_ms);

void status_test_chip_id_fetch(
    volatile bool* cancel_requested,
    char row[STATUS_PAGE_LINE_LEN]);

int status_test_format_battery(
    const bwm_battery_info_t* snapshot,
    char lines[STATUS_PAGE_MAX_LINES][STATUS_PAGE_LINE_LEN]);

int status_test_cep_fetch(
    volatile bool* cancel_requested,
    char rows[STATUS_PAGE_ROWS_PER_PAGE][STATUS_PAGE_LINE_LEN],
    bwm_battery_info_t* battery);

int status_test_wifi_fetch(
    volatile bool* cancel_requested,
    char rows[STATUS_PAGE_ROWS_PER_PAGE][STATUS_PAGE_LINE_LEN]);

void status_test_ble_fetch(
    volatile bool* cancel_requested,
    char rows[STATUS_PAGE_ROWS_PER_PAGE][STATUS_PAGE_LINE_LEN]);

#endif // STATUS_PAGE_TESTS_H
