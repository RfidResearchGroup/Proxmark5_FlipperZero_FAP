#pragma once

#include <dialogs/dialogs.h>
#include <storage/storage.h>

typedef bool (*SavedFileFilter)(Storage* storage, const char* path);

bool saved_file_browse(
    Storage* storage,
    DialogsApp* dialogs,
    FuriString* selected,
    const char* directory,
    const char* extension,
    const Icon* icon,
    SavedFileFilter filter);
