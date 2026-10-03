#ifndef STATUS_PAGE_TESTS_H
#define STATUS_PAGE_TESTS_H

#include "status_page.h"

// StatusPageFetchFn implementations - one per "Functions Menu" test item.
// See status_page.h for the contract each must follow.

int status_test_capabilities_fetch(
    volatile bool* cancel_requested,
    char lines[STATUS_PAGE_MAX_LINES][STATUS_PAGE_LINE_LEN],
    int* out_line_count);

int status_test_ping_fetch(
    volatile bool* cancel_requested,
    char lines[STATUS_PAGE_MAX_LINES][STATUS_PAGE_LINE_LEN],
    int* out_line_count);

int status_test_flash_chip_fetch(
    volatile bool* cancel_requested,
    char lines[STATUS_PAGE_MAX_LINES][STATUS_PAGE_LINE_LEN],
    int* out_line_count);

// Needs CMD_PM5_BWM_GET_BATTERY on the PM5 side (proxmark3-RRG branch
// pm5-bwm-battery-telemetry, not yet merged upstream as of writing) - shows a
// clean "N/A" line against any PM5 build that doesn't have it yet.
int status_test_battery_fetch(
    volatile bool* cancel_requested,
    char lines[STATUS_PAGE_MAX_LINES][STATUS_PAGE_LINE_LEN],
    int* out_line_count);

// Needs CMD_CEP_STATUS on the PM5 side (proxmark3-RRG branch pm5-cep-status,
// not yet merged upstream as of writing) - shows a clean "N/A" line against
// any PM5 build that doesn't have it yet. Always returns PM3_SUCCESS from the
// firmware side - a timeout/bad-reply here means an old firmware without this
// command, not "feature unsupported".
int status_test_cep_fetch(
    volatile bool* cancel_requested,
    char lines[STATUS_PAGE_MAX_LINES][STATUS_PAGE_LINE_LEN],
    int* out_line_count);

#endif // STATUS_PAGE_TESTS_H
