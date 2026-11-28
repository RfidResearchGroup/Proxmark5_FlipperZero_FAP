#ifndef BWM_POWER_PAGE_H
#define BWM_POWER_PAGE_H

#include <gui/view.h>

typedef struct BwmPowerPage BwmPowerPage;

BwmPowerPage* bwm_power_page_create(void);
void bwm_power_page_stop_worker(BwmPowerPage* page);
void bwm_power_page_free(BwmPowerPage* page);
View* bwm_power_page_get_view(BwmPowerPage* page);
void bwm_power_page_tick(BwmPowerPage* page);
void bwm_power_page_start(BwmPowerPage* page);

#endif
