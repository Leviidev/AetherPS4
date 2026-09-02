#include "sysmodules_import.h"

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <miniz.h>
#include <miniz_zip.h>

namespace {

void SetMessage(BachataSysModulesImportResult* out, const char* message) {
    std::snprintf(out->message, sizeof(out->message), "%s", message);
}

// Rejects absolute paths and any ".." component -- a malicious or malformed ZIP entry could
// otherwise write outside dest_dir (the classic "zip slip" vulnerability). std::filesystem's
// lexically_normal doesn't resolve symlinks, so this is a pre-extraction structural check on
// the *name* only, not a guarantee about the resolved target -- sufficient here since dest_dir
// is always this app's own sandboxed sys_modules directory, never attacker-controlled.
bool IsSafeRelativePath(const std::filesystem::path& relative) {
    if (relative.empty() || relative.is_absolute()) {
        return false;
    }
    for (const auto& part : relative) {
        if (part == "..") {
            return false;
        }
    }
    return true;
}

} // namespace

int bachata_sysmodules_import_zip(const char* zip_path, const char* dest_dir,
                                   BachataSysModulesImportResult* out) {
    if (out == nullptr) {
        return 2;
    }
    out->status = 2;
    out->files_extracted = 0;
    SetMessage(out, "");

    if (zip_path == nullptr || dest_dir == nullptr) {
        SetMessage(out, "Missing zip path or destination.");
        return out->status;
    }

    std::error_code ec;
    std::filesystem::create_directories(dest_dir, ec);
    if (ec) {
        SetMessage(out, "Could not create the sys_modules directory.");
        return out->status;
    }

    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, zip_path, 0)) {
        out->status = 1;
        SetMessage(out, "This file isn't a valid ZIP archive.");
        return out->status;
    }

    const std::filesystem::path dest_root = std::filesystem::path(dest_dir);
    const mz_uint num_files = mz_zip_reader_get_num_files(&zip);
    int extracted = 0;
    bool saw_unsafe_entry = false;

    for (mz_uint i = 0; i < num_files; ++i) {
        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            continue;
        }

        char name_buf[1024] = {};
        const mz_uint name_len = mz_zip_reader_get_filename(&zip, i, name_buf, sizeof(name_buf));
        if (name_len == 0 || name_len >= sizeof(name_buf)) {
            continue;
        }

        const std::filesystem::path relative(name_buf);
        if (!IsSafeRelativePath(relative)) {
            saw_unsafe_entry = true;
            continue;
        }

        const std::filesystem::path target = dest_root / relative;
        std::filesystem::create_directories(target.parent_path(), ec);
        if (ec) {
            continue;
        }

        if (mz_zip_reader_extract_to_file(&zip, i, target.c_str(), 0)) {
            ++extracted;
        }
    }

    mz_zip_reader_end(&zip);

    out->files_extracted = extracted;
    if (extracted == 0) {
        out->status = saw_unsafe_entry ? 3 : 1;
        SetMessage(out, saw_unsafe_entry ? "The archive contains unsafe file paths and nothing "
                                            "else usable."
                                          : "No files could be extracted from this archive.");
        return out->status;
    }

    out->status = 0;
    SetMessage(out, "");
    return out->status;
}
