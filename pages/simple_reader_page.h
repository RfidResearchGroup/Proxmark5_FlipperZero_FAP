#ifndef SIMPLE_READER_PAGE_H
#define SIMPLE_READER_PAGE_H

#include <gui/view.h>
#include "../readers/simple_readers.h"

typedef struct SimpleReaderPage SimpleReaderPage;

typedef void (*SimpleReaderDetailsCallback)(void* context);

SimpleReaderPage* simple_reader_page_create(SimpleReaderDetailsCallback on_details, void* context);
bool simple_reader_page_format_details(SimpleReaderPage* page, FuriString* text);
void simple_reader_page_start(SimpleReaderPage* page, const SimpleReader* reader);
void simple_reader_page_tick(SimpleReaderPage* page);
void simple_reader_page_stop(SimpleReaderPage* page);
void simple_reader_page_stop_worker(SimpleReaderPage* page);
void simple_reader_page_free(SimpleReaderPage* page);
View* simple_reader_page_get_view(SimpleReaderPage* page);

#endif
