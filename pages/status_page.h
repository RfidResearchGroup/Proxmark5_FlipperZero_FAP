#ifndef STATUS_PAGE_H
#define STATUS_PAGE_H

#include <gui/view.h>
#include <stdbool.h>

// One reusable "send a command, show the result as label:value lines" page,
// so a new hardware self-test is one fetch function instead of a full page
// module wired through fmps_cxt.h/.c + operate_page.c (see CLAUDE.md).

#define STATUS_PAGE_MAX_LINES 8
#define STATUS_PAGE_LINE_LEN 32
#define STATUS_PAGE_VISIBLE_LINES 3

typedef struct StatusPage StatusPage;

// Invoked when the user leaves the page via the on-screen "Back" (left) button.
typedef void (*StatusPageBackCallback)(void* context);

// Runs on the worker thread. Must do its own SendCommandNG/wait-for-response
// (copy read_hitag2_page's cancel-aware wait pattern, not the bare
// WaitForResponseTimeout, so the test stays cancellable). Fill in up to
// STATUS_PAGE_MAX_LINES formatted "label: value" strings and *out_line_count;
// this may still be done on a non-PM3_SUCCESS return (e.g. one line saying
// "N/A (no flash)" on a timeout) for a graceful degraded display - the page
// only falls back to a generic Timeout/Cancelled/Failed message when
// *out_line_count is left at 0. Returns a PM3_* result code.
typedef int (*StatusPageFetchFn)(
    volatile bool* cancel_requested,
    char lines[STATUS_PAGE_MAX_LINES][STATUS_PAGE_LINE_LEN],
    int* out_line_count);

StatusPage* status_page_create(StatusPageBackCallback back_callback, void* back_callback_context);
void status_page_free(StatusPage* status_page);

View* status_page_get_view(StatusPage* status_page);

// Sets the title, spawns the worker thread, and runs `fetch`. Safe to call
// again on the same page instance for a different test.
void status_page_start(StatusPage* status_page, const char* title, StatusPageFetchFn fetch);
// Non-blocking: only raises the cancel flag, same contract as
// read_hitag2_page_stop() - never call furi_thread_join() from here, this
// runs on the GUI thread via the view's previous-callback / back button.
void status_page_stop(StatusPage* status_page);

#endif // STATUS_PAGE_H
