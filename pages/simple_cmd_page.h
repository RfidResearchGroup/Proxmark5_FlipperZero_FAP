#ifndef SIMPLE_CMD_PAGE_H
#define SIMPLE_CMD_PAGE_H

#include <gui/view.h>
#include <stdbool.h>

typedef enum {
    SimpleCmdPing = 0,
    SimpleCmdVersion,
    SimpleCmdHf14a,
} SimpleCmdKind;

typedef struct SimpleCmdPage SimpleCmdPage;

typedef void (*SimpleCmdSaveRequestCallback)(void* context);

SimpleCmdPage* simple_cmd_page_create(void);
void simple_cmd_page_free(SimpleCmdPage* page);

View* simple_cmd_page_get_view(SimpleCmdPage* page);

void simple_cmd_page_set_save_request_callback(
    SimpleCmdPage* page,
    SimpleCmdSaveRequestCallback callback,
    void* context);

/** Suggested filename (no extension), e.g. UID hex. */
const char* simple_cmd_page_default_name(SimpleCmdPage* page);

/** Save as /ext/nfc/<name>.nfc. name may include or omit .nfc. */
bool simple_cmd_page_save_as(SimpleCmdPage* page, const char* name);

void simple_cmd_page_set_status(SimpleCmdPage* page, const char* status);

void simple_cmd_page_start(SimpleCmdPage* page, SimpleCmdKind kind);
void simple_cmd_page_stop(SimpleCmdPage* page);

#endif
