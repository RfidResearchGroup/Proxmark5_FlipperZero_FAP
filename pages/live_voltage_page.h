#ifndef LIVE_VOLTAGE_PAGE_H
#define LIVE_VOLTAGE_PAGE_H

#include <gui/view.h>

typedef struct LiveVoltagePage LiveVoltagePage;

LiveVoltagePage* live_voltage_page_create(void (*return_to_antenna)(void*), void* context);
void live_voltage_page_tick(LiveVoltagePage* page);
void live_voltage_page_stop_worker(LiveVoltagePage* page);
void live_voltage_page_free(LiveVoltagePage* page);
View* live_voltage_page_get_view(LiveVoltagePage* page);

#endif
