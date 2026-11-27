#include <dialogs/dialogs.h>
#include <flipper_format/flipper_format.h>
#include <furi.h>
#include <fmps_cxt_icons.h>
#include <storage/storage.h>
#include "saved_tag_read.h"

#define TAG_READ_DIR APP_DATA_PATH("tag-reads")
#define TAG_READ_EXT ".pm5tag"
#define TAG_READ_TYPE "Proxmark5 tag read"
#define TAG_READ_VERSION 1U
#define TAG_READ_FILE_MAX 1024U

static const char* const tag_types[] = {
    [SavedTagHf14a] = "HF 14A",
    [SavedTagHitag2] = "Hitag2",
    [SavedTagT5577] = "T5577",
};

static bool uid_length_valid(uint32_t length) {
    return length == 4 || length == 7 || length == 10;
}

static bool read_hex(FlipperFormat* file, const char* key, uint8_t* data, uint32_t length) {
    uint32_t count;
    return flipper_format_get_value_count(file, key, &count) && count == length &&
           flipper_format_read_hex(file, key, data, length);
}

static bool read_card(FlipperFormat* file, iso14a_card_select_t* card) {
    uint32_t uid_len, ats_len;
    if(!flipper_format_get_value_count(file, "UID", &uid_len) || !uid_length_valid(uid_len) ||
       !flipper_format_read_hex(file, "UID", card->uid, uid_len) ||
       !read_hex(file, "ATQA", card->atqa, sizeof(card->atqa)) ||
       !read_hex(file, "SAK", &card->sak, 1) ||
       !flipper_format_read_uint32(file, "ATS length", &ats_len, 1) || ats_len > UINT8_MAX) {
        return false;
    }
    card->uidlen = uid_len;
    card->ats_len = ats_len;
    return ats_len == 0 || read_hex(file, "ATS", card->ats, ats_len);
}

static uint32_t hitag2_uid_from_bytes(const uint8_t uid[4]) {
    return (uint32_t)uid[0] << 24 | (uint32_t)uid[1] << 16 | (uint32_t)uid[2] << 8 | uid[3];
}

static bool read_record(FlipperFormat* file, SavedTagRead* tag) {
    uint32_t version;
    FuriString* value = furi_string_alloc();
    bool ok = flipper_format_read_header(file, value, &version) && version == TAG_READ_VERSION &&
              furi_string_equal(value, TAG_READ_TYPE) &&
              flipper_format_read_string(file, "Tag type", value);
    if(ok && furi_string_equal(value, tag_types[SavedTagHf14a])) {
        tag->type = SavedTagHf14a;
        ok = read_card(file, &tag->hf14a);
    } else if(ok && furi_string_equal(value, tag_types[SavedTagHitag2])) {
        uint8_t uid[4];
        tag->type = SavedTagHitag2;
        ok = read_hex(file, "UID", uid, sizeof(uid));
        if(ok) tag->hitag2_uid = hitag2_uid_from_bytes(uid);
    } else if(ok && furi_string_equal(value, tag_types[SavedTagT5577])) {
        tag->type = SavedTagT5577;
        ok = read_hex(file, "Block 0", tag->t5577_config, sizeof(tag->t5577_config));
    } else {
        ok = false;
    }
    furi_string_free(value);
    return ok;
}

static bool write_record(FlipperFormat* file, const SavedTagRead* tag) {
    if(!flipper_format_write_header_cstr(file, TAG_READ_TYPE, TAG_READ_VERSION) ||
       !flipper_format_write_string_cstr(file, "Tag type", tag_types[tag->type])) return false;

    if(tag->type == SavedTagHf14a) {
        const iso14a_card_select_t* card = &tag->hf14a;
        uint32_t ats_len = card->ats_len;
        return flipper_format_write_hex(file, "UID", card->uid, card->uidlen) &&
               flipper_format_write_hex(file, "ATQA", card->atqa, sizeof(card->atqa)) &&
               flipper_format_write_hex(file, "SAK", &card->sak, 1) &&
               flipper_format_write_uint32(file, "ATS length", &ats_len, 1) &&
               (ats_len == 0 || flipper_format_write_hex(file, "ATS", card->ats, ats_len));
    }
    if(tag->type == SavedTagHitag2) {
        uint32_t uid = tag->hitag2_uid;
        const uint8_t bytes[] = {uid >> 24, uid >> 16, uid >> 8, uid};
        return flipper_format_write_hex(file, "UID", bytes, sizeof(bytes));
    }
    return flipper_format_write_hex(file, "Block 0", tag->t5577_config, sizeof(tag->t5577_config));
}

static FuriString* tag_directory(Storage* storage) {
    FuriString* dir = furi_string_alloc_set(TAG_READ_DIR);
    storage_common_resolve_path_and_ensure_app_directory(storage, dir);
    if(storage_simply_mkdir(storage, furi_string_get_cstr(dir))) return dir;
    furi_string_free(dir);
    return NULL;
}

static bool write_tag(Storage* storage, const char* path, const SavedTagRead* tag) {
    FlipperFormat* file = flipper_format_file_alloc(storage);
    bool saved = false;
    if(flipper_format_file_open_new(file, path)) {
        bool written = write_record(file, tag);
        saved = flipper_format_file_close(file) && written;
        if(!saved) storage_common_remove(storage, path);
    }
    flipper_format_free(file);
    return saved;
}

static bool read_tag(Storage* storage, const char* path, SavedTagRead* tag) {
    FileInfo info;
    if(storage_common_stat(storage, path, &info) != FSE_OK || info.size > TAG_READ_FILE_MAX) {
        return false;
    }
    FlipperFormat* file = flipper_format_file_alloc(storage);
    bool loaded = flipper_format_file_open_existing(file, path) && read_record(file, tag);
    flipper_format_free(file);
    return loaded;
}

void saved_tag_read_save(const SavedTagRead* tag) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FuriString* dir = tag_directory(storage);
    FuriString* name = furi_string_alloc();
    bool saved = false;
    if(dir && (tag->type != SavedTagHf14a || uid_length_valid(tag->hf14a.uidlen))) {
        storage_get_next_filename(
            storage, furi_string_get_cstr(dir), tag_types[tag->type], TAG_READ_EXT, name, 20);
        furi_string_cat_printf(dir, "/%s" TAG_READ_EXT, furi_string_get_cstr(name));
        saved = write_tag(storage, furi_string_get_cstr(dir), tag);
    }
    if(dir) furi_string_free(dir);
    furi_record_close(RECORD_STORAGE);

    DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
    if(saved) {
        DialogMessage* message = dialog_message_alloc();
        dialog_message_set_header(message, "Tag read saved", 64, 17, AlignCenter, AlignCenter);
        dialog_message_set_text(message, furi_string_get_cstr(name), 64, 37, AlignCenter, AlignCenter);
        dialog_message_set_buttons(message, NULL, "OK", NULL);
        dialog_message_show(dialogs, message);
        dialog_message_free(message);
    } else {
        dialog_message_show_storage_error(dialogs, "Cannot save tag read");
    }
    furi_record_close(RECORD_DIALOGS);
    furi_string_free(name);
}

static void format_tag(FuriString* text, const SavedTagRead* tag) {
    furi_string_printf(text, "Saved %s\n", tag_types[tag->type]);
    if(tag->type == SavedTagHf14a) {
        const iso14a_card_select_t* card = &tag->hf14a;
        furi_string_cat_str(text, card->uidlen == 10 ? "UID:\n" : "UID: ");
        for(size_t i = 0; i < card->uidlen; ++i) furi_string_cat_printf(text, "%02X", card->uid[i]);
        furi_string_cat_printf(text, "\nATQA: %02X%02X\nSAK: %02X", card->atqa[1], card->atqa[0], card->sak);
        if(card->ats_len) {
            furi_string_cat_str(text, "\nATS (incl. CRC):");
            for(size_t i = 0; i < card->ats_len; ++i) {
                if(i % 8 == 0) furi_string_push_back(text, '\n');
                furi_string_cat_printf(text, "%02X%s", card->ats[i], i % 8 == 7 ? "" : " ");
            }
        }
    } else if(tag->type == SavedTagHitag2) {
        furi_string_cat_printf(text, "UID: %08lX", (unsigned long)tag->hitag2_uid);
    } else {
        const uint8_t* config = tag->t5577_config;
        furi_string_cat_printf(text, "Block 0: %02X%02X%02X%02X",
                              config[0], config[1], config[2], config[3]);
    }
}

bool saved_tag_read_browse(FuriString* text) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FuriString* dir = tag_directory(storage);
    DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
    bool loaded = false;
    if(dir) {
        DialogsFileBrowserOptions options;
        dialog_file_browser_set_basic_options(&options, TAG_READ_EXT, &I_pm5_tag_file_10px);
        options.base_path = furi_string_get_cstr(dir);
        options.hide_ext = true;
        FuriString* selected = furi_string_alloc();
        if(dialog_file_browser_show(dialogs, selected, dir, &options)) {
            SavedTagRead tag = {0};
            loaded = read_tag(storage, furi_string_get_cstr(selected), &tag);
            if(loaded) format_tag(text, &tag);
            else dialog_message_show_storage_error(dialogs, "Invalid PM5 tag file");
        }
        furi_string_free(selected);
        furi_string_free(dir);
    } else {
        dialog_message_show_storage_error(dialogs, "Cannot open tag reads");
    }
    furi_record_close(RECORD_DIALOGS);
    furi_record_close(RECORD_STORAGE);
    return loaded;
}
