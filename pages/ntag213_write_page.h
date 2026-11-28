#ifndef NTAG21X_WRITE_PAGE_H
#define NTAG21X_WRITE_PAGE_H

#include <gui/view.h>
#include "../readers/ntag213.h"

typedef struct Ntag21xWritePage Ntag21xWritePage;
typedef void (*Ntag21xWritePageCallback)(void* context);

Ntag21xWritePage* ntag21x_write_page_alloc(
    Ntag21xWritePageCallback on_text, Ntag21xWritePageCallback on_done, void* context);
void ntag21x_write_page_free(Ntag21xWritePage* page);
View* ntag21x_write_page_get_view(Ntag21xWritePage* page);
View* ntag21x_write_page_get_input_view(Ntag21xWritePage* page);
View* ntag21x_write_page_get_url_view(Ntag21xWritePage* page);
void ntag21x_write_page_edit(
    Ntag21xWritePage* page, const Ntag21xPages* tag, Ntag21xWriteFormat format);
void ntag21x_write_page_confirm(Ntag21xWritePage* page);
bool ntag21x_write_page_is_url(const Ntag21xWritePage* page);
void ntag21x_write_page_wait(Ntag21xWritePage* page);

#endif
