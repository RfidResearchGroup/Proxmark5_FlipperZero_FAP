#ifndef STATUS_PAGE_H
#define STATUS_PAGE_H

#include <gui/view.h>
#include <stdbool.h>

#define STATUS_PAGE_MAX_LINES 8
#define STATUS_PAGE_LINE_LEN 40
#define STATUS_PAGE_PAGE_COUNT 4
#define STATUS_PAGE_ROWS_PER_PAGE 4

typedef struct StatusPage StatusPage;

StatusPage* status_page_create(void);
void status_page_stop_worker(StatusPage* status_page);
void status_page_free(StatusPage* status_page);

View* status_page_get_view(StatusPage* status_page);

// Start a refresh, or queue it while a cancelled scan finishes.
void status_page_start(StatusPage* status_page);
// Cancel the scan and clear any queued refresh without joining the worker.
void status_page_stop(StatusPage* status_page);

#endif // STATUS_PAGE_H
