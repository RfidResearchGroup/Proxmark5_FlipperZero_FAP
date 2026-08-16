#ifndef CLASSIC_DUMP_PAGE_H
#define CLASSIC_DUMP_PAGE_H

#include <gui/view.h>
#include <stdbool.h>

typedef struct ClassicDumpPage ClassicDumpPage;

typedef void (*ClassicDumpSaveRequestCallback)(void* context);
typedef void (*ClassicDumpBackRequestCallback)(void* context);
typedef void (*ClassicDumpMfkeyRequestCallback)(void* context);

ClassicDumpPage* classic_dump_page_create(void);
void classic_dump_page_free(ClassicDumpPage* page);

View* classic_dump_page_get_view(ClassicDumpPage* page);

void classic_dump_page_set_save_request_callback(
    ClassicDumpPage* page,
    ClassicDumpSaveRequestCallback callback,
    void* context);

void classic_dump_page_set_back_request_callback(
    ClassicDumpPage* page,
    ClassicDumpBackRequestCallback callback,
    void* context);

void classic_dump_page_set_mfkey_request_callback(
    ClassicDumpPage* page,
    ClassicDumpMfkeyRequestCallback callback,
    void* context);

/** Reset UI to idle (ready to Start). Stops any worker. */
void classic_dump_page_reset(ClassicDumpPage* page);

const char* classic_dump_page_default_name(ClassicDumpPage* page);
bool classic_dump_page_save_as(ClassicDumpPage* page, const char* name);
bool classic_dump_page_can_save(ClassicDumpPage* page);

void classic_dump_page_start(ClassicDumpPage* page);
void classic_dump_page_stop(ClassicDumpPage* page);

#endif
