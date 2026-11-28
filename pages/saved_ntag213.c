#include <dialogs/dialogs.h>
#include <furi.h>
#include <flipper_format/flipper_format.h>
#include <storage/storage.h>
#include <string.h>
#include "saved_ntag213.h"

// Keep existing NTAG213 dumps in the same folder.
#define NTAG_DUMP_DIR APP_DATA_PATH("ntag213")
#define DUMP_EXT ".bin"

// Proxmark3 mfu_dump_t prefix, without page data.
typedef struct {
    uint8_t version[NTAG21X_VERSION_BYTES];
    uint8_t reserved[3];
    uint8_t last_page;
    uint8_t signature[32];
    uint8_t counter_tearing[3][4];
} MfuDumpHeader;

_Static_assert(sizeof(MfuDumpHeader) == 56, "Unexpected MFU dump header size");

static FuriString* ntag_dump_directory(Storage* storage) {
    FuriString* dir = furi_string_alloc_set(NTAG_DUMP_DIR);
    storage_common_resolve_path_and_ensure_app_directory(storage, dir);
    if(storage_simply_mkdir(storage, furi_string_get_cstr(dir))) return dir;
    furi_string_free(dir);
    return NULL;
}

static bool write_dump(
    Storage* storage, const char* path, const Ntag21xPages* pages, const Ntag21xModel* model) {
    MfuDumpHeader header = {.last_page = model->read_pages - 1U};
    memcpy(header.version, pages->version, sizeof(header.version));
    size_t size = model->read_pages * NTAG21X_PAGE_BYTES;
    File* file = storage_file_alloc(storage);
    bool saved = false;
    if(storage_file_open(file, path, FSAM_WRITE, FSOM_CREATE_NEW)) {
        bool written = storage_file_write(file, &header, sizeof(header)) == sizeof(header) &&
                       storage_file_write(file, pages->bytes, size) == size &&
                       storage_file_sync(file);
        saved = storage_file_close(file) && written;
        if(!saved) storage_common_remove(storage, path);
    }
    storage_file_free(file);
    return saved;
}

static bool read_dump(Storage* storage, const char* path, Ntag21xPages* pages) {
    File* file = storage_file_alloc(storage);
    MfuDumpHeader header;
    bool loaded = false;
    if(storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING) &&
       storage_file_read(file, &header, sizeof(header)) == sizeof(header)) {
        const Ntag21xModel* model = ntag21x_model_for_version(header.version);
        unsigned page_count = header.last_page + 1U;
        if(model && (page_count == model->read_pages || page_count == model->total_pages) &&
           storage_file_size(file) == sizeof(header) + page_count * NTAG21X_PAGE_BYTES) {
            size_t size = model->read_pages * NTAG21X_PAGE_BYTES;
            memcpy(pages->version, header.version, sizeof(pages->version));
            loaded = storage_file_read(file, pages->bytes, size) == size &&
                     ntag21x_pages_valid(pages);
        }
    }
    storage_file_free(file);
    return loaded;
}

void saved_ntag21x_save(const Ntag21xPages* pages) {
    const Ntag21xModel* model = ntag21x_model_for_version(pages->version);
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FuriString* dir = ntag_dump_directory(storage);
    bool saved = false;
    char filename[32 + sizeof(DUMP_EXT)];
    if(dir && model && ntag21x_pages_valid(pages)) {
        uint8_t uid[NTAG21X_UID_BYTES];
        ntag21x_pages_uid(pages, uid);
        char name[2 * NTAG21X_UID_BYTES + 1];
        snprintf(name, sizeof(name), "%02X%02X%02X%02X%02X%02X%02X",
                 uid[0], uid[1], uid[2], uid[3], uid[4], uid[5], uid[6]);

        FuriString* available = furi_string_alloc();
        storage_get_next_filename(storage, furi_string_get_cstr(dir), name, DUMP_EXT, available, 32);
        snprintf(filename, sizeof(filename), "%s" DUMP_EXT, furi_string_get_cstr(available));
        furi_string_cat_printf(dir, "/%s", filename);
        saved = write_dump(storage, furi_string_get_cstr(dir), pages, model);
        furi_string_free(available);
    }
    if(dir) furi_string_free(dir);
    furi_record_close(RECORD_STORAGE);

    DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
    if(saved) {
        DialogMessage* message = dialog_message_alloc();
        char header[24];
        snprintf(header, sizeof(header), "%s saved", model->name);
        dialog_message_set_header(message, header, 64, 17, AlignCenter, AlignCenter);
        char details[sizeof(filename) + sizeof("\nPages 0-225")];
        snprintf(details, sizeof(details), "%s\nPages 0-%u", filename, model->read_pages - 1U);
        dialog_message_set_text(message, details, 64, 37, AlignCenter, AlignCenter);
        dialog_message_set_buttons(message, NULL, "OK", NULL);
        dialog_message_show(dialogs, message);
        dialog_message_free(message);
    } else {
        dialog_message_show_storage_error(dialogs, "Cannot save NTAG dump");
    }
    furi_record_close(RECORD_DIALOGS);
}

static bool read_native(Storage* storage, const char* path, Ntag21xPages* pages) {
    FileInfo info;
    if(storage_common_stat(storage, path, &info) != FSE_OK || info.size > 8192U) return false;

    FlipperFormat* file = flipper_format_file_alloc(storage);
    FuriString* value = furi_string_alloc();
    uint32_t version, total, read;
    uint8_t uid[NTAG21X_UID_BYTES];
    bool loaded = flipper_format_file_open_existing(file, path) &&
                  flipper_format_read_header(file, value, &version) && version == 4 &&
                  furi_string_equal(value, "Flipper NFC device") &&
                  flipper_format_read_string(file, "Device type", value) &&
                  furi_string_equal(value, "NTAG/Ultralight") &&
                  flipper_format_read_hex(file, "UID", uid, sizeof(uid)) &&
                  flipper_format_read_string(file, "NTAG/Ultralight type", value) &&
                  flipper_format_read_hex(
                      file, "Mifare version", pages->version, sizeof(pages->version));
    const Ntag21xModel* model = loaded ? ntag21x_model_for_version(pages->version) : NULL;
    loaded = model && furi_string_equal(value, model->name) &&
             flipper_format_read_uint32(file, "Pages total", &total, 1) &&
             total == model->total_pages &&
             flipper_format_read_uint32(file, "Pages read", &read, 1) &&
             read >= model->read_pages && read <= total;
    for(unsigned page = 0; loaded && page < model->read_pages; page++) {
        char key[16];
        snprintf(key, sizeof(key), "Page %u", page);
        loaded = flipper_format_read_hex(
            file, key, pages->bytes + page * NTAG21X_PAGE_BYTES, NTAG21X_PAGE_BYTES);
    }
    if(loaded) {
        uint8_t memory_uid[NTAG21X_UID_BYTES];
        ntag21x_pages_uid(pages, memory_uid);
        loaded = ntag21x_pages_valid(pages) && memcmp(uid, memory_uid, sizeof(uid)) == 0;
    }
    furi_string_free(value);
    flipper_format_free(file);
    return loaded;
}

bool saved_ntag21x_browse(Ntag21xPages* pages, bool native) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FuriString* dir = native ? furi_string_alloc_set(EXT_PATH("nfc")) : ntag_dump_directory(storage);
    DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
    bool loaded = false;
    if(dir) {
        DialogsFileBrowserOptions options;
        dialog_file_browser_set_basic_options(&options, native ? ".nfc" : DUMP_EXT, NULL);
        options.base_path = furi_string_get_cstr(dir);
        options.hide_ext = true;

        FuriString* selected = furi_string_alloc();
        if(dialog_file_browser_show(dialogs, selected, dir, &options)) {
            const char* path = furi_string_get_cstr(selected);
            memset(pages, 0, sizeof(*pages));
            loaded = native ? read_native(storage, path, pages) : read_dump(storage, path, pages);
            if(!loaded) dialog_message_show_storage_error(dialogs, "Invalid NTAG file");
        }
        furi_string_free(selected);
        furi_string_free(dir);
    } else {
        dialog_message_show_storage_error(dialogs, "Cannot open NTAG files");
    }
    furi_record_close(RECORD_DIALOGS);
    furi_record_close(RECORD_STORAGE);
    return loaded;
}
