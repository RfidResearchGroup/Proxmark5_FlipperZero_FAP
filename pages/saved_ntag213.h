#ifndef SAVED_NTAG21X_H
#define SAVED_NTAG21X_H

#include "../readers/ntag213.h"

void saved_ntag21x_save(const Ntag21xPages* pages);
bool saved_ntag21x_browse(Ntag21xPages* pages, bool native);

#endif
