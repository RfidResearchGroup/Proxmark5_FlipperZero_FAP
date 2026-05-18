#ifndef proxmark5_COM_H
#define proxmark5_COM_H

#include <stdbool.h>

void proxmark5_com_init(void);
void proxmark5_com_deinit(void);
bool proxmark5_com_handshake(void);

#endif // proxmark5_COM_H