#ifndef NTAG21X_H
#define NTAG21X_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NTAG21X_UID_BYTES 7U
#define NTAG21X_VERSION_BYTES 8U
#define NTAG21X_PAGE_BYTES 4U
#define NTAG21X_MAX_READ_PAGES 226U
#define NTAG21X_TEXT_MAX 247U
#define NTAG21X_URL_MAX 249U

typedef struct {
    uint8_t bytes[NTAG21X_MAX_READ_PAGES * NTAG21X_PAGE_BYTES];
    uint8_t version[NTAG21X_VERSION_BYTES];
} Ntag21xPages;

typedef struct {
    const char* name;
    uint8_t version[NTAG21X_VERSION_BYTES];
    uint8_t read_pages;
    uint8_t total_pages;
    uint8_t cc_size;
} Ntag21xModel;

const Ntag21xModel* ntag21x_model_for_version(const uint8_t version[NTAG21X_VERSION_BYTES]);
void ntag21x_pages_uid(const Ntag21xPages* pages, uint8_t uid[NTAG21X_UID_BYTES]);
bool ntag21x_pages_valid(const Ntag21xPages* pages);

typedef enum {
    Ntag21xWriteText,
    Ntag21xWriteUrl,
    Ntag21xWriteTextAndUrl,
} Ntag21xWriteFormat;

bool ntag21x_write_can_encode(
    const Ntag21xPages* pages, Ntag21xWriteFormat format, const char* const values[3], bool https);

size_t ntag21x_write_value_max(const Ntag21xPages* pages, Ntag21xWriteFormat format);

typedef enum {
    Ntag21xWriteChecking,
    Ntag21xWritePages,
    Ntag21xWriteVerifying,
} Ntag21xWriteStage;

typedef enum {
    Ntag21xWriteVerified,
    Ntag21xWriteLinkError,
    Ntag21xWriteTagChanged,
    Ntag21xWriteProtected,
    Ntag21xWriteUnsupported,
    Ntag21xWriteIncomplete,
    Ntag21xWriteNoMemory,
} Ntag21xWriteResult;

typedef void (*Ntag21xWriteProgress)(
    Ntag21xWriteStage stage, size_t done, size_t total, void* context);

Ntag21xWriteResult ntag21x_write_ndef(
    const Ntag21xPages* expected,
    Ntag21xWriteFormat format,
    const char* const values[3],
    bool https,
    Ntag21xWriteProgress progress,
    void* context,
    bool* cleanup_ok);

#endif
