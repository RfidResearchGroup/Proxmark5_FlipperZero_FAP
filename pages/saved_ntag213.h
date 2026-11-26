#ifndef SAVED_NTAG213_H
#define SAVED_NTAG213_H

#include "../readers/ntag213.h"

void saved_ntag213_save(const Ntag213Pages* pages);
bool saved_ntag213_browse(Ntag213Pages* pages, bool native);

#endif
