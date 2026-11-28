#ifndef NTAG21X_H
#define NTAG21X_H

#include <stdbool.h>
#include <stdint.h>

#define NTAG21X_UID_BYTES 7U
#define NTAG21X_VERSION_BYTES 8U
#define NTAG21X_PAGE_BYTES 4U
#define NTAG21X_MAX_READ_PAGES 226U

typedef struct {
    uint8_t bytes[NTAG21X_MAX_READ_PAGES * NTAG21X_PAGE_BYTES];
    uint8_t version[NTAG21X_VERSION_BYTES];
} Ntag21xPages;

typedef struct {
    const char* name;
    uint8_t version[NTAG21X_VERSION_BYTES];
    uint8_t read_pages;
    uint8_t total_pages;
} Ntag21xModel;

const Ntag21xModel* ntag21x_model_for_version(const uint8_t version[NTAG21X_VERSION_BYTES]);
void ntag21x_pages_uid(const Ntag21xPages* pages, uint8_t uid[NTAG21X_UID_BYTES]);
bool ntag21x_pages_valid(const Ntag21xPages* pages);

#endif
