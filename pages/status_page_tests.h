#ifndef STATUS_PAGE_TESTS_H
#define STATUS_PAGE_TESTS_H

#include "status_page.h"
#include "pm3_cmd.h"

int status_test_capabilities_fetch(
    volatile bool* cancel_requested,
    char lines[STATUS_PAGE_MAX_LINES][STATUS_PAGE_LINE_LEN],
    int* out_line_count);

bool status_test_ping_fetch(uint32_t* rtt_ms);

void status_test_chip_id_fetch(
    volatile bool* cancel_requested,
    char row[STATUS_PAGE_LINE_LEN]);

// Use the CEP snapshot when available, or ask older firmware for battery data.
int status_test_battery_fetch(
    volatile bool* cancel_requested,
    const bwm_battery_info_t* snapshot,
    char lines[STATUS_PAGE_MAX_LINES][STATUS_PAGE_LINE_LEN],
    int* out_line_count);

// Older builds can still use CMD_PM5_BWM_GET_BATTERY.
int status_test_cep_fetch(
    volatile bool* cancel_requested,
    char rows[STATUS_PAGE_ROWS_PER_PAGE][STATUS_PAGE_LINE_LEN],
    bwm_battery_info_t* battery);

#endif // STATUS_PAGE_TESTS_H
