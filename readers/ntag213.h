#ifndef NTAG213_H
#define NTAG213_H

#include <stdbool.h>
#include <stdint.h>

#define NTAG213_UID_BYTES 7U
#define NTAG213_VERSION_BYTES 8U
#define NTAG213_PAGE_BYTES 4U
#define NTAG213_READ_PAGES 40U
#define NTAG213_TOTAL_PAGES 45U
#define NTAG213_READ_BYTES (NTAG213_READ_PAGES * NTAG213_PAGE_BYTES)

typedef struct {
    uint8_t bytes[NTAG213_READ_BYTES];
} Ntag213Pages;

extern const uint8_t ntag213_version[NTAG213_VERSION_BYTES];

void ntag213_pages_uid(const Ntag213Pages* pages, uint8_t uid[NTAG213_UID_BYTES]);
bool ntag213_pages_valid(const Ntag213Pages* pages);

#endif
