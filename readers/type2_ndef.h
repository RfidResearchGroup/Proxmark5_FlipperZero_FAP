#ifndef TYPE2_NDEF_H
#define TYPE2_NDEF_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    Type2NdefEmpty,
    Type2NdefText,
    Type2NdefUri,
    Type2NdefTextAndUri,
    Type2NdefOther,
    Type2NdefInvalid,
} Type2NdefResult;

Type2NdefResult type2_ndef_read(
    const uint8_t* data, size_t size, char* text, size_t text_size);

#endif
