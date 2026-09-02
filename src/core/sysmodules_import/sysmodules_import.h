#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BachataSysModulesImportResult {
    int status; // 0 OK, 1 BAD_ZIP, 2 IO_ERROR, 3 UNSAFE_PATH
    int files_extracted;
    char message[256];
} BachataSysModulesImportResult;

// Extracts a ZIP of dumped PS4 system modules (.sprx files, optionally nested under a
// per-game serial subdirectory) directly into dest_dir, preserving the ZIP's own internal
// directory structure -- so a ZIP made by zipping a "sys_modules" folder's contents (loose
// .sprx files, or GameSerial/ subfolders for per-game overrides) lands exactly where
// EmulatorSettings::GetSysModulesDir() expects it once dest_dir *is* that directory.
// Rejects any entry whose path would escape dest_dir (../, absolute paths).
int bachata_sysmodules_import_zip(const char* zip_path, const char* dest_dir,
                                   BachataSysModulesImportResult* out);

#ifdef __cplusplus
}
#endif
