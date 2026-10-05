#ifndef STATUS_PAGE_H
#define STATUS_PAGE_H

#include <gui/view.h>
#include <stdbool.h>

// Paginated hardware-status dashboard: 3 fixed pages (Core/Power/Link),
// flipped with Right. One status_page_start() runs all 5 self-tests once.

#define STATUS_PAGE_MAX_LINES 8
#define STATUS_PAGE_LINE_LEN 32
#define STATUS_PAGE_PAGE_COUNT 3
#define STATUS_PAGE_ROWS_PER_PAGE 4

typedef struct StatusPage StatusPage;

// Invoked when the user leaves the page via the on-screen "Back" (left) button.
typedef void (*StatusPageBackCallback)(void* context);

StatusPage* status_page_create(StatusPageBackCallback back_callback, void* back_callback_context);
void status_page_free(StatusPage* status_page);

View* status_page_get_view(StatusPage* status_page);

// Spawns the worker thread, which runs all 5 status tests and fills the
// dashboard. Safe to call again on the same instance (e.g. reopening the page).
void status_page_start(StatusPage* status_page);
// Non-blocking: only raises the cancel flag, same contract as
// read_hitag2_page_stop() - never call furi_thread_join() from here.
void status_page_stop(StatusPage* status_page);

#endif // STATUS_PAGE_H
