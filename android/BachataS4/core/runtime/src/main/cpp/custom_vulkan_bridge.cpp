#include <adrenotools/driver.h>

#include <android/log.h>
#include <dlfcn.h>
#include <sys/stat.h>

#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <string>

namespace {
constexpr char LogTag[] = "BachataVulkan";

std::string LibraryDirectory() {
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&LibraryDirectory), &info) == 0 || info.dli_fname == nullptr) {
        return {};
    }
    const std::string path{info.dli_fname};
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string{} : path.substr(0, slash + 1);
}
} // namespace

extern "C" __attribute__((visibility("default"))) void* bachata_open_custom_vulkan() {
    static std::once_flag once;
    static void* vulkan_handle = nullptr;
    std::call_once(once, [] {
        const char* driver_dir = std::getenv("BACHATA_VULKAN_DRIVER_DIR");
        const char* driver_name = std::getenv("BACHATA_VULKAN_DRIVER_NAME");
        const char* tmp_dir = std::getenv("BACHATA_VULKAN_TMPDIR");
        const std::string hook_dir = LibraryDirectory();
        if (driver_dir == nullptr || driver_name == nullptr || tmp_dir == nullptr || hook_dir.empty()) {
            __android_log_print(ANDROID_LOG_ERROR, LogTag, "Custom Vulkan environment is incomplete");
            std::fprintf(stderr, "[Bachata.Vulkan] <Error> Custom Vulkan environment is incomplete\n");
            return;
        }
        mkdir(tmp_dir, 0700);
        vulkan_handle = adrenotools_open_libvulkan(
            RTLD_NOW | RTLD_LOCAL,
            ADRENOTOOLS_DRIVER_CUSTOM,
            tmp_dir,
            hook_dir.c_str(),
            driver_dir,
            driver_name,
            nullptr,
            nullptr);
        __android_log_print(
            vulkan_handle == nullptr ? ANDROID_LOG_ERROR : ANDROID_LOG_INFO,
            LogTag,
            "Turnip custom driver %s",
            vulkan_handle == nullptr ? "failed" : "loaded");
        std::fprintf(
            stderr,
            "[Bachata.Vulkan] <%s> Turnip custom driver %s/%s %s\n",
            vulkan_handle == nullptr ? "Error" : "Info",
            driver_dir,
            driver_name,
            vulkan_handle == nullptr ? "failed" : "loaded");
    });
    return vulkan_handle;
}
