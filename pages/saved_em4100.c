#include <dialogs/dialogs.h>
#include <flipper_format/flipper_format.h>
#include <furi.h>
#include <fmps_cxt_icons.h>
#include <storage/storage.h>
#include "../readers/simple_readers.h"
#include "saved_em4100.h"

#define RFID_DIR EXT_PATH("lfrfid")
#define RFID_EXTENSION ".rfid"
#define RFID_FILE_TYPE "Flipper RFID key"
#define RFID_FILE_VERSION 1U
#define RFID_FILE_MAX 1024U
#define EM4100_KEY_TYPE "EM4100"

static void saved_message(const char* title, const char* text) {
    DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
    DialogMessage* message = dialog_message_alloc();
    dialog_message_set_header(message, title, 64, 12, AlignCenter, AlignCenter);
    dialog_message_set_text(message, text, 64, 31, AlignCenter, AlignCenter);
    dialog_message_set_buttons(message, NULL, "OK", NULL);
    dialog_message_show(dialogs, message);
    dialog_message_free(message);
    furi_record_close(RECORD_DIALOGS);
}

static void format_id(const uint8_t* id, char* text, size_t size) {
    snprintf(text, size, "%02X%02X%02X%02X%02X", id[0], id[1], id[2], id[3], id[4]);
}

static bool read_em4100(Storage* storage, const char* path, uint8_t* id) {
    FileInfo info;
    if(storage_common_stat(storage, path, &info) != FSE_OK || info.size > RFID_FILE_MAX) {
        return false;
    }

    FlipperFormat* file = flipper_format_file_alloc(storage);
    FuriString* value = furi_string_alloc();
    uint32_t version = 0;
    uint32_t count = 0;

    bool ok = flipper_format_file_open_existing(file, path) &&
              flipper_format_read_header(file, value, &version) &&
              furi_string_equal(value, RFID_FILE_TYPE) && version == RFID_FILE_VERSION &&
              flipper_format_read_string(file, "Key type", value) &&
              furi_string_equal(value, EM4100_KEY_TYPE) &&
              flipper_format_get_value_count(file, "Data", &count) && count == EM4100_ID_BYTES &&
              flipper_format_read_hex(file, "Data", id, EM4100_ID_BYTES);

    furi_string_free(value);
    flipper_format_free(file);
    return ok;
}

static bool write_em4100(Storage* storage, const char* path, const uint8_t* id) {
    FlipperFormat* file = flipper_format_file_alloc(storage);
    bool opened = flipper_format_file_open_new(file, path);
    bool written = opened && flipper_format_write_header_cstr(
                                 file, RFID_FILE_TYPE, RFID_FILE_VERSION) &&
                   flipper_format_write_string_cstr(file, "Key type", EM4100_KEY_TYPE) &&
                   flipper_format_write_hex(file, "Data", id, EM4100_ID_BYTES);
    bool closed = opened && flipper_format_file_close(file);
    flipper_format_free(file);

    uint8_t saved[EM4100_ID_BYTES];
    if(written && closed && read_em4100(storage, path, saved) &&
       memcmp(saved, id, sizeof(saved)) == 0) {
        return true;
    }

    if(opened) storage_common_remove(storage, path);
    return false;
}

void saved_em4100_save(const uint8_t* id) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    char text[2 * EM4100_ID_BYTES + 1];
    format_id(id, text, sizeof(text));

    const char* title = "Save failed";
    const char* body = "Check the SD card";
    if(storage_simply_mkdir(storage, RFID_DIR)) {
        FuriString* path = furi_string_alloc_printf(RFID_DIR "/EM4100-%s" RFID_EXTENSION, text);
        const char* filename = furi_string_get_cstr(path);
        uint8_t previous[EM4100_ID_BYTES];
        if(read_em4100(storage, filename, previous) &&
           memcmp(previous, id, sizeof(previous)) == 0) {
            title = "Already saved";
            body = text;
        } else if(write_em4100(storage, filename, id)) {
            title = "EM4100 saved";
            body = text;
        }
        furi_string_free(path);
    }

    furi_record_close(RECORD_STORAGE);
    saved_message(title, body);
}

static void select_first_rfid_file(Storage* storage, FuriString* path) {
    File* directory = storage_file_alloc(storage);
    if(storage_dir_open(directory, RFID_DIR)) {
        FuriString* candidate = furi_string_alloc();
        FileInfo info;
        char name[256];
        uint8_t id[EM4100_ID_BYTES];
        while(storage_dir_read(directory, &info, name, sizeof(name))) {
            if(file_info_is_dir(&info) || name[0] == '.') continue;
            furi_string_printf(candidate, RFID_DIR "/%s", name);
            if(!furi_string_end_withi_str(candidate, RFID_EXTENSION) ||
               !read_em4100(storage, furi_string_get_cstr(candidate), id)) continue;
            furi_string_set(path, candidate);
            break;
        }
        furi_string_free(candidate);
        storage_dir_close(directory);
    }
    storage_file_free(directory);
}

void saved_em4100_browse(void) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    if(storage_simply_mkdir(storage, RFID_DIR)) {
        DialogsFileBrowserOptions options;
        // 125 kHz file icon from Flipper firmware assets.
        dialog_file_browser_set_basic_options(&options, RFID_EXTENSION, &I_rfid_file_10px);
        options.base_path = RFID_DIR;
        options.hide_ext = true;

        DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
        FuriString* path = furi_string_alloc_set(RFID_DIR);
        select_first_rfid_file(storage, path);
        FuriString* selected = furi_string_alloc();
        bool opened = dialog_file_browser_show(dialogs, selected, path, &options);
        furi_string_free(path);
        furi_record_close(RECORD_DIALOGS);

        uint8_t id[EM4100_ID_BYTES];
        if(opened && read_em4100(storage, furi_string_get_cstr(selected), id)) {
            char text[2 * EM4100_ID_BYTES + 1];
            format_id(id, text, sizeof(text));
            saved_message("Saved EM4100", text);
        } else if(opened) {
            saved_message("Unable to open", "Select an EM4100 file");
        }
        furi_string_free(selected);
    } else {
        saved_message("Unable to open", "Check the SD card");
    }

    furi_record_close(RECORD_STORAGE);
}
