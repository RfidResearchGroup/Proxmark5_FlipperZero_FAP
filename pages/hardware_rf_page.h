#pragma once

#include <gui/view.h>

typedef enum {
    HardwareRfThreshold,
    HardwareRfFrequency,
    HardwareRfReadTest,
} HardwareRfMode;

typedef struct HardwareRfPage HardwareRfPage;

HardwareRfPage* hardware_rf_page_alloc(void);
View* hardware_rf_page_view(HardwareRfPage* page);
void hardware_rf_page_open(HardwareRfPage* page, HardwareRfMode mode);
void hardware_rf_page_stop_worker(HardwareRfPage* page);
void hardware_rf_page_free(HardwareRfPage* page);
