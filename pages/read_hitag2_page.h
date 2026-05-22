#ifndef READ_HITAG2_PAGE_H
#define READ_HITAG2_PAGE_H

#include <gui/view.h>

typedef struct ReadHitag2Page ReadHitag2Page;

ReadHitag2Page* read_hitag2_page_create(void);
void read_hitag2_page_free(ReadHitag2Page* read_hitag2_page);

View* read_hitag2_page_get_view(ReadHitag2Page* read_hitag2_page);

void read_hitag2_page_start(ReadHitag2Page* read_hitag2_page);
void read_hitag2_page_stop(ReadHitag2Page* read_hitag2_page);

#endif // READ_HITAG2_PAGE_H
