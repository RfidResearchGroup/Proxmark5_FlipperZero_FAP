#pragma once

#include <stdbool.h>
#include <stdint.h>

#define HARDWARE_RF_TEST_ATTEMPTS 5U
#define HARDWARE_RF_THRESHOLD_MIN 1
#define HARDWARE_RF_THRESHOLD_MAX 63

enum { HardwareRfFieldOffUnconfirmed = 1 };

typedef struct {
    uint8_t selected;
    uint8_t version;
    uint8_t read;
} HardwareRfTest;

int hardware_rf_threshold(bool apply, uint8_t* value, const volatile bool* cancel);
int hardware_rf_lf_divisor(bool apply, uint16_t* divisor);
int hardware_rf_read_test(HardwareRfTest* test, const volatile bool* cancel);
