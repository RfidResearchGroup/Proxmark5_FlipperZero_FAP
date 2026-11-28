#ifndef HF14A_TRACE_PAGE_H
#define HF14A_TRACE_PAGE_H

#include <gui/view.h>

typedef struct Hf14aTracePage Hf14aTracePage;

Hf14aTracePage* hf14a_trace_page_create(void);
void hf14a_trace_page_wait(Hf14aTracePage* page);
void hf14a_trace_page_free(Hf14aTracePage* page);
void hf14a_trace_page_start(Hf14aTracePage* page, uint32_t previous_view);
bool hf14a_trace_page_browse(Hf14aTracePage* page, uint32_t previous_view);
View* hf14a_trace_page_view(Hf14aTracePage* page);
uint32_t hf14a_trace_page_previous(void* context);

#endif
