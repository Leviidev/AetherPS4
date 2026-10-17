// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// libhuskps4's one export: the PS4 core as a Husk console module (husk_console_abi.h). It wraps
// the embeddable C API (shadps4_ios_api.h) and replaces each seam Husk owns: executable memory
// comes from Husk's JIT region (Core::SetJitHost), frames go to Husk's CAMetalLayer
// (Frontend::SetHostSurface), Vulkan is the MoltenVK Husk names, and pads come from Husk.

#include "platform/husk/husk_console_abi.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include <SDL3/SDL_events.h>

#include "common/phone_path.h"
#include "core/ios/ios_jit_allocator.h"
#include "platform/husk/host_surface.h"
#include "platform/ios/shadps4_ios_api.h"

#if !defined(AETHER_PHONE_PATH)
#error "libhuskps4 is built on the phone path (iOS, or macOS with AETHER_MAC_PHONE_PATH)"
#endif

namespace {

// Code buffers are 64 MiB, and a full one is replaced rather than refilled, so a few can be alive
// at once while threads move to the newest; plus a 16 KiB dispatcher, trampolines, HLE veneers and
// the shader-walker pool. The phone's region is 512 MiB.
constexpr size_t kJitCeilingBytes = size_t{448} << 20;

HuskHostV1 g_host{};
std::atomic<int> g_state{HUSK_CONSOLE_IDLE};
std::atomic<bool> g_initialized{false};
std::mutex g_launch_mutex;
std::thread g_game_thread;

void HostLog(const char* fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (g_host.log_line != nullptr) {
        g_host.log_line(line);
    } else {
        std::fprintf(stderr, "%s\n", line);
    }
}

void OnFirstFrame() {
    int booting = HUSK_CONSOLE_BOOTING;
    g_state.compare_exchange_strong(booting, HUSK_CONSOLE_RUNNING);
    HostLog("huskps4: first frame presented");
}

int Init(const HuskHostV1* host, const char* user_dir) {
    if (host == nullptr || user_dir == nullptr || host->version != HUSK_CONSOLE_ABI_VERSION) {
        return -1;
    }
    if (g_initialized.load()) {
        return 0;
    }
    std::memcpy(&g_host, host, std::min<size_t>(host->struct_size, sizeof(g_host)));
    Frontend::SetHuskHosted(true);
    if (g_host.jit_alloc == nullptr || g_host.jit_free == nullptr || g_host.jit_size == 0) {
        HostLog("huskps4: the host gave no JIT region");
        return -1;
    }
    Core::SetJitHost(Core::JitHost{
        .alloc = g_host.jit_alloc,
        .free = g_host.jit_free,
        .delta = g_host.jit_delta,
        .rw_base = g_host.jit_rw_base,
        .size = g_host.jit_size,
    });
    // Absent (zero) from hosts built before the keyboard: dialogs are then answered for the player.
    Frontend::SetHostTextRequester(
        reinterpret_cast<Frontend::HostTextRequester>(g_host.request_text));
    if (g_host.vulkan_library_path != nullptr && g_host.vulkan_library_path[0] != '\0') {
        // vk_platform.cpp and sdl_window.cpp load this library directly, with no loader.
        setenv("AETHER_VULKAN_LIBRARY", g_host.vulkan_library_path, 1);
    }
    ShadPS4Options options{};
    options.user_dir = user_dir;
    options.show_fps = 0;
    options.fullscreen = -1;
    options.network_enabled = 0;
    // Husk captures stdout into its own log, so lines go to the PS4 log file only, and by default
    // only warnings and worse: the core's trace output costs whole frames on a phone.
    shadps4_set_log_console(0);
    if (shadps4_init(&options) != 0) {
        HostLog("huskps4: core init failed (user dir %s)", user_dir);
        return -1;
    }
    const char* log_filter = std::getenv("AETHER_LOG_FILTER");
    shadps4_set_log_filter(log_filter != nullptr ? log_filter : "*:Warning");
    shadps4_register_first_frame_callback(&OnFirstFrame);
    g_initialized.store(true);
    HostLog("huskps4: ready (user dir %s, JIT region %zu MiB, ceiling %zu MiB)", user_dir,
            g_host.jit_size >> 20, kJitCeilingBytes >> 20);
    return 0;
}

std::string ResolveEboot(const char* game) {
    std::filesystem::path path(game);
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
        path /= "eboot.bin";
    }
    return path.string();
}

int Launch(const char* game, const char* args, void* metal_layer, int width_px, int height_px,
           float scale) {
    std::scoped_lock lock{g_launch_mutex};
    if (!g_initialized.load() || game == nullptr || metal_layer == nullptr || width_px <= 0 ||
        height_px <= 0) {
        return -1;
    }
    if (g_state.load() != HUSK_CONSOLE_IDLE) {
        HostLog("huskps4: one game per process; already launched");
        return -1;
    }
    Frontend::SetHostSurface(Frontend::HostSurface{
        .metal_layer = metal_layer,
        .width_px = width_px,
        .height_px = height_px,
        .scale = scale > 0.0f ? scale : 1.0f,
    });
    const std::string eboot = ResolveEboot(game);
    const std::string joined_args = args != nullptr ? args : "";
    g_state.store(HUSK_CONSOLE_BOOTING);
    HostLog("huskps4: launching %s (%dx%d px)", eboot.c_str(), width_px, height_px);
    g_game_thread = std::thread([eboot, joined_args] {
        if (shadps4_prepare_window_with_args(eboot.c_str(), joined_args.c_str()) != 0) {
            HostLog("huskps4: the game could not be prepared: %s", eboot.c_str());
            g_state.store(HUSK_CONSOLE_FAILED);
            return;
        }
        shadps4_run_loop();
        HostLog("huskps4: the game ended");
        g_state.store(HUSK_CONSOLE_STOPPED);
    });
    g_game_thread.detach();
    return 0;
}

int State() {
    const int state = g_state.load();
    if (state == HUSK_CONSOLE_RUNNING && shadps4_is_paused()) {
        return HUSK_CONSOLE_PAUSED;
    }
    return state;
}

uint64_t Frames() {
    return shadps4_get_presented_frame_count();
}

void SetPaused(int paused) {
    const int state = g_state.load();
    if (state != HUSK_CONSOLE_RUNNING) {
        return;
    }
    if ((shadps4_is_paused() != 0) != (paused != 0)) {
        shadps4_toggle_pause();
    }
}

void SetPad(const HuskPadV1* pad) {
    // The pad state reads the PS4 process clock, which exists once the game is running.
    if (pad == nullptr || g_state.load() != HUSK_CONSOLE_RUNNING) {
        return;
    }
    shadps4_apply_touch_input(pad->buttons, pad->left_x, pad->left_y, pad->right_x,
                              pad->right_y, pad->l2, pad->r2);
}

void Resize(int width_px, int height_px) {
    if (width_px <= 0 || height_px <= 0) {
        return;
    }
    Frontend::ResizeHostSurface(width_px, height_px);
    SDL_Event event{};
    event.type = SDL_EVENT_WINDOW_RESIZED;
    event.window.data1 = width_px;
    event.window.data2 = height_px;
    SDL_PushEvent(&event);
}

void Stop() {
    if (g_state.load() == HUSK_CONSOLE_IDLE) {
        return;
    }
    shadps4_stop();
}

int TakePendingRestart(char* path, int path_size, char* args, int args_size) {
    return shadps4_take_pending_restart(path, path_size, args, args_size);
}

const HuskConsoleModuleV1 kModule{
    .struct_size = sizeof(HuskConsoleModuleV1),
    .version = HUSK_CONSOLE_ABI_VERSION,
    .platform = "ps4",
    .jit_ceiling_bytes = kJitCeilingBytes,
    .init = Init,
    .launch = Launch,
    .state = State,
    .frames = Frames,
    .set_paused = SetPaused,
    .set_pad = SetPad,
    .resize = Resize,
    .stop = Stop,
    .take_pending_restart = TakePendingRestart,
};

} // namespace

extern "C" __attribute__((visibility("default"))) const HuskConsoleModuleV1*
husk_console_module_v1(void) {
    return &kModule;
}
