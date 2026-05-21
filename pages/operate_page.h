#ifndef OPERATE_PAGE_H
#define OPERATE_PAGE_H

#include <gui/modules/submenu.h>

typedef struct {
    Submenu* submenu;
} OperatePage;

OperatePage* operate_page_create(void);
void operate_page_free(OperatePage* operate_page);

#endif // OPERATE_PAGE_H
