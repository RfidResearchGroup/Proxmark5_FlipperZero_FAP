#include <dialogs/dialogs.h>
#include <furi.h>
#include <storage/storage.h>
#include "saved_ntag213.h"

#define NTAG213_DIR APP_DATA_PATH("ntag213")
#define DUMP_EXT ".bin"

// Proxmark3 mfu_dump_t prefix, without page data.
typedef struct {
    uint8_t version[NTAG213_VERSION_BYTES];
    uint8_t reserved[3];
    uint8_t last_page;
    uint8_t signature[32];
    uint8_t counter_tearing[3][4];
} MfuDumpHeader;

_Static_assert(sizeof(MfuDumpHeader) == 56, "Unexpected MFU dump header size");

static FuriString* ntag213_directory(Storage* storage) {
    FuriString* dir = furi_string_alloc_set(NTAG213_DIR);
    storage_common_resolve_path_and_ensure_app_directory(storage, dir);
    if(storage_simply_mkdir(storage, furi_string_get_cstr(dir))) return dir;
    furi_string_free(dir);
    return NULL;
}

static bool write_dump(Storage* storage, const char* path, const Ntag213Pages* pages) {
    MfuDumpHeader header = {.last_page = NTAG213_READ_PAGES - 1};
    memcpy(header.version, ntag213_version, sizeof(header.version));
    File* file = storage_file_alloc(storage);
    bool saved = false;
    if(storage_file_open(file, path, FSAM_WRITE, FSOM_CREATE_NEW)) {
        bool written = storage_file_write(file, &header, sizeof(header)) == sizeof(header) &&
                       storage_file_write(file, pages->bytes, sizeof(pages->bytes)) ==
                           sizeof(pages->bytes) &&
                       storage_file_sync(file);
        saved = storage_file_close(file) && written;
        if(!saved) storage_common_remove(storage, path);
    }
    storage_file_free(file);
    return saved;
}

static bool read_dump(Storage* storage, const char* path, Ntag213Pages* pages) {
    File* file = storage_file_alloc(storage);
    MfuDumpHeader header;
    bool loaded = false;
    if(storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING) &&
       storage_file_read(file, &header, sizeof(header)) == sizeof(header) &&
       memcmp(header.version, ntag213_version, sizeof(header.version)) == 0) {
        unsigned page_count = header.last_page + 1U;
        loaded = (page_count == NTAG213_READ_PAGES || page_count == NTAG213_TOTAL_PAGES) &&
                 storage_file_size(file) == sizeof(header) + page_count * NTAG213_PAGE_BYTES &&
                 storage_file_read(file, pages->bytes, sizeof(pages->bytes)) ==
                     sizeof(pages->bytes) &&
                 ntag213_pages_valid(pages);
    }
    storage_file_free(file);
    return loaded;
}

void saved_ntag213_save(const Ntag213Pages* pages) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FuriString* dir = ntag213_directory(storage);
    bool saved = false;
    char filename[32 + sizeof(DUMP_EXT)];
    if(dir && ntag213_pages_valid(pages)) {
        uint8_t uid[NTAG213_UID_BYTES];
        ntag213_pages_uid(pages, uid);
        char name[2 * NTAG213_UID_BYTES + 1];
        snprintf(name, sizeof(name), "%02X%02X%02X%02X%02X%02X%02X",
                 uid[0], uid[1], uid[2], uid[3], uid[4], uid[5], uid[6]);

        FuriString* available = furi_string_alloc();
        storage_get_next_filename(storage, furi_string_get_cstr(dir), name, DUMP_EXT, available, 32);
        snprintf(filename, sizeof(filename), "%s" DUMP_EXT, furi_string_get_cstr(available));
        furi_string_cat_printf(dir, "/%s", filename);
        saved = write_dump(storage, furi_string_get_cstr(dir), pages);
        furi_string_free(available);
    }
    if(dir) furi_string_free(dir);
    furi_record_close(RECORD_STORAGE);

    DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
    if(saved) {
        DialogMessage* message = dialog_message_alloc();
        dialog_message_set_header(message, "NTAG213 saved", 64, 17, AlignCenter, AlignCenter);
        char details[sizeof(filename) + sizeof("\nPages 0-39")];
        snprintf(details, sizeof(details), "%s\nPages 0-39", filename);
        dialog_message_set_text(message, details, 64, 37, AlignCenter, AlignCenter);
        dialog_message_set_buttons(message, NULL, "OK", NULL);
        dialog_message_show(dialogs, message);
        dialog_message_free(message);
    } else {
        dialog_message_show_storage_error(dialogs, "Cannot save NTAG213");
    }
    furi_record_close(RECORD_DIALOGS);
}

bool saved_ntag213_browse(Ntag213Pages* pages) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FuriString* dir = ntag213_directory(storage);
    DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
    bool loaded = false;
    if(dir) {
        DialogsFileBrowserOptions options;
        dialog_file_browser_set_basic_options(&options, DUMP_EXT, NULL);
        options.base_path = furi_string_get_cstr(dir);
        options.hide_ext = true;

        FuriString* selected = furi_string_alloc();
        if(dialog_file_browser_show(dialogs, selected, dir, &options)) {
            loaded = read_dump(storage, furi_string_get_cstr(selected), pages);
            if(!loaded) dialog_message_show_storage_error(dialogs, "Invalid NTAG file");
        }
        furi_string_free(selected);
        furi_string_free(dir);
    } else {
        dialog_message_show_storage_error(dialogs, "Cannot open NTAG213");
    }
    furi_record_close(RECORD_DIALOGS);
    furi_record_close(RECORD_STORAGE);
    return loaded;
}
