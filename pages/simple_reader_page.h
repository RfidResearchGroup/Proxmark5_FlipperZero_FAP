#ifndef SIMPLE_READER_PAGE_H
#define SIMPLE_READER_PAGE_H

#include <gui/view.h>
#include "../readers/simple_readers.h"

typedef struct SimpleReaderPage SimpleReaderPage;

typedef void (*SimpleReaderDetailsCallback)(void* context);

SimpleReaderPage* simple_reader_page_create(
    SimpleReaderDetailsCallback on_details, SimpleReaderDetailsCallback on_actions, void* context);
bool simple_reader_page_get_live_data(
    SimpleReaderPage* page, const SimpleReader* reader, uint8_t* data);
bool simple_reader_page_format_details(SimpleReaderPage* page, FuriString* text);
bool simple_reader_page_show_saved(SimpleReaderPage* page, const SimpleReader* reader, const uint8_t* data);
bool simple_reader_page_is_saved(SimpleReaderPage* page);
void simple_reader_page_start(SimpleReaderPage* page, const SimpleReader* reader);
void simple_reader_page_tick(SimpleReaderPage* page);
void simple_reader_page_stop(SimpleReaderPage* page);
void simple_reader_page_stop_worker(SimpleReaderPage* page);
void simple_reader_page_free(SimpleReaderPage* page);
View* simple_reader_page_get_view(SimpleReaderPage* page);

#endif
