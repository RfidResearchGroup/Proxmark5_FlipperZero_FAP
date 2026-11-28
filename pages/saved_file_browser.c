#include "saved_file_browser.h"

bool saved_file_browse(
    Storage* storage,
    DialogsApp* dialogs,
    FuriString* selected,
    const char* directory,
    const char* extension,
    const Icon* icon,
    SavedFileFilter filter) {
    FuriString* initial = furi_string_alloc_set(directory);
    File* folder = storage_file_alloc(storage);
    if(storage_dir_open(folder, directory)) {
        FuriString* candidate = furi_string_alloc();
        FileInfo info;
        char name[256];
        bool found = false;
        while(storage_dir_read(folder, &info, name, sizeof(name))) {
            if(file_info_is_dir(&info) || !name[0] || name[0] == '.') continue;
            furi_string_printf(candidate, "%s/%s", directory, name);
            if(!furi_string_end_withi_str(candidate, extension)) continue;
            if(found && furi_string_cmpi(candidate, initial) >= 0) continue;
            if(filter && !filter(storage, furi_string_get_cstr(candidate))) continue;
            furi_string_set(initial, candidate);
            found = true;
        }
        furi_string_free(candidate);
    }
    storage_dir_close(folder);
    storage_file_free(folder);

    DialogsFileBrowserOptions options = {0};
    dialog_file_browser_set_basic_options(&options, extension, icon);
    options.base_path = directory;
    options.hide_dot_files = true;
    bool opened = dialog_file_browser_show(dialogs, selected, initial, &options);
    furi_string_free(initial);
    return opened;
}
