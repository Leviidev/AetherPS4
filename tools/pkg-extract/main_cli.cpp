// aetherps4-pkg-extract: standalone CLI wrapper around the ported Bachata PKG
// extractor (see pkg_extractor.h) for the desktop AetherPS4 launcher. Not
// part of the emulator core itself -- this only turns an encrypted .pkg into
// a plain extracted game directory (sce_sys/, eboot.bin, ...) that shadps4's
// existing "-g <path>" flag can load directly, the same way it already loads
// an unpacked game folder.
#include "pkg_extractor.h"

#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <fcntl.h>
#include <unistd.h>

namespace {

void print_usage(const char* argv0) {
    std::fprintf(stderr, "usage: %s probe <pkg-path>\n", argv0);
    std::fprintf(stderr, "       %s extract <pkg-path> <out-dir> [passcode]\n", argv0);
}

void on_progress(void*, uint64_t done, uint64_t total, const char* file) {
    // One machine-readable line per update; the launcher parses this to
    // drive a progress bar. Also perfectly readable run directly by a human.
    std::printf("PROGRESS %llu %llu %s\n", static_cast<unsigned long long>(done),
                static_cast<unsigned long long>(total), file ? file : "");
    std::fflush(stdout);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        print_usage(argv[0]);
        return 64; // EX_USAGE
    }

    const std::string_view command = argv[1];
    const char* pkg_path = argv[2];

    const int fd = open(pkg_path, O_RDONLY);
    if (fd < 0) {
        std::fprintf(stderr, "error: cannot open %s\n", pkg_path);
        return 66; // EX_NOINPUT
    }

    if (command == "probe") {
        BachataPkgProbe probe{};
        const int status = bachata_pkg_probe(fd, &probe);
        close(fd);
        std::printf("STATUS %d\n", status);
        std::printf("CONTENT_ID %s\n", probe.content_id);
        std::printf("TITLE_HINT %s\n", probe.title_hint);
        std::printf("PACKAGE_SIZE %llu\n", static_cast<unsigned long long>(probe.package_size));
        std::printf("PFS_IMAGE_SIZE %llu\n", static_cast<unsigned long long>(probe.pfs_image_size));
        if (probe.message[0]) {
            std::printf("MESSAGE %s\n", probe.message);
        }
        return status;
    }

    if (command == "extract") {
        if (argc < 4) {
            close(fd);
            print_usage(argv[0]);
            return 64;
        }
        const char* out_dir = argv[3];
        const char* passcode = argc >= 5 ? argv[4] : nullptr;
        const int status = bachata_pkg_extract(fd, out_dir, passcode, on_progress, nullptr);
        close(fd);
        std::printf("STATUS %d\n", status);
        return status;
    }

    close(fd);
    print_usage(argv[0]);
    return 64;
}
