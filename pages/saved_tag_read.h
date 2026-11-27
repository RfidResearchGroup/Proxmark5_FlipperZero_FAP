#ifndef SAVED_TAG_READ_H
#define SAVED_TAG_READ_H

#include <furi.h>
#include "pm3_cmd.h"

typedef enum {
    SavedTagHf14a,
    SavedTagHitag2,
    SavedTagT5577,
} SavedTagType;

typedef struct {
    SavedTagType type;
    union {
        iso14a_card_select_t hf14a;
        uint32_t hitag2_uid;
        uint8_t t5577_config[4];
    };
} SavedTagRead;

void saved_tag_read_save(const SavedTagRead* tag);
bool saved_tag_read_browse(FuriString* text);

#endif
