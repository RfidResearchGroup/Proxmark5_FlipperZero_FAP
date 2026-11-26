#ifndef ANTENNA_CHECK_PAGE_H
#define ANTENNA_CHECK_PAGE_H

#include <gui/view.h>

typedef struct AntennaCheckPage AntennaCheckPage;

AntennaCheckPage* antenna_check_page_create(void);
void antenna_check_page_tick(AntennaCheckPage* page);
void antenna_check_page_stop_worker(AntennaCheckPage* page);
void antenna_check_page_free(AntennaCheckPage* page);
View* antenna_check_page_get_view(AntennaCheckPage* page);

#endif
