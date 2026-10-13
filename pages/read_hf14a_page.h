#ifndef READ_HF14A_PAGE_H
#define READ_HF14A_PAGE_H

#include <gui/view.h>

typedef struct ReadHf14aPage ReadHf14aPage;

// Invoked when the user leaves the page via the on-screen "Back" (left) button.
typedef void (*ReadHf14aPageBackCallback)(void* context);

ReadHf14aPage*
    read_hf14a_page_create(ReadHf14aPageBackCallback back_callback, void* back_callback_context);
void read_hf14a_page_free(ReadHf14aPage* read_hf14a_page);

View* read_hf14a_page_get_view(ReadHf14aPage* read_hf14a_page);

void read_hf14a_page_start(ReadHf14aPage* read_hf14a_page);
void read_hf14a_page_stop(ReadHf14aPage* read_hf14a_page);

#endif // READ_HF14A_PAGE_H
