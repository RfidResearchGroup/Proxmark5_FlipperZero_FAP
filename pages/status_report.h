#ifndef STATUS_REPORT_H
#define STATUS_REPORT_H

#include <gui/view.h>

typedef struct StatusReport StatusReport;

StatusReport* status_report_create(void);
void status_report_stop_worker(StatusReport* report);
void status_report_free(StatusReport* report);
View* status_report_get_view(StatusReport* report);
void status_report_tick(StatusReport* report);
void status_report_start(StatusReport* report);

#endif
