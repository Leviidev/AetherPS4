// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "platform/ios/shadps4_ios_api.h"

#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>

#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_video.h>

#include "common/frame_presented_flag.h"
#include "common/key_manager.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/sigsys_trap.h"
#include "common/singleton.h"
#include "core/emulator_settings.h"
#include "core/emulator_state.h"
#include "core/file_sys/fs.h"
#include "core/ios/ios_jit_allocator.h"
#include "core/ipc/ipc.h"
#include "core/libraries/pad/pad.h"
#include "core/loader/elf.h"
#include "core/user_settings.h"
#include "emulator.h"
#include "input/controller.h"

namespace {

std::once_flag g_init_flag;
bool g_init_ok = false;

// Mirrors main.cpp's "resolve a path or a game ID" behavior (see FindGameByID usage
// there): try the string as a literal path first, then search EmulatorSettings'
// configured install directories for a matching game ID.
std::optional<std::filesystem::path> ResolveEbootPath(std::string_view game_path_or_id) {
    std::filesystem::path candidate(game_path_or_id);
    if (std::filesystem::exists(candidate)) {
        return candidate;
    }
    constexpr int max_depth = 5;
    for (const auto& install_dir : EmulatorSettings.GetGameInstallDirs()) {
        if (auto found = Common::FS::FindGameByID(install_dir, std::string(game_path_or_id),
                                                    max_depth)) {
            return found;
        }
    }
    return std::nullopt;
}

} // namespace

extern "C" int shadps4_init(const ShadPS4Options* options) {
    std::call_once(g_init_flag, [&] {
        Common::InstallBachataSigsysTrap();

        // SetUserPath requires the directory to already exist (see path_util.cpp) --
        // std::filesystem::create_directories is a no-op if it's already there.
        if (options && options->user_dir && options->user_dir[0] != '\0') {
            std::error_code ec;
            std::filesystem::create_directories(options->user_dir, ec);
            if (!ec) {
                Common::FS::SetUserPath(Common::FS::PathType::UserDir, options->user_dir);
            }
        }

        Common::Log::Setup("shadps4.log");

        IPC::Instance().Init();

        auto emu_state = std::make_shared<EmulatorState>();
        EmulatorState::SetInstance(emu_state);
        UserSettings.Load();

        auto key_manager = KeyManager::GetInstance();
        key_manager->LoadFromFile();

        auto emu_settings = std::make_shared<EmulatorSettingsImpl>();
        EmulatorSettingsImpl::SetInstance(emu_settings);
        emu_settings->Load();
        Common::Log::g_should_append |= EmulatorSettings.IsLogAppend();

        if (options) {
            if (options->show_fps) {
                EmulatorSettings.SetShowFpsCounter(true);
            }
            if (options->fullscreen == 0) {
                EmulatorSettings.SetFullScreen(false);
            } else if (options->fullscreen == 1) {
                EmulatorSettings.SetFullScreen(true);
            }
            if (options->network_enabled) {
                EmulatorSettings.SetShadNetEnabled(true);
                EmulatorSettings.SetConnectedToNetwork(true);
            }
        }

        g_init_ok = true;
    });
    return g_init_ok ? 0 : -1;
}

extern "C" int shadps4_prepare_window(const char* eboot_path) {
    if (!g_init_ok || !eboot_path) {
        return -1;
    }

    auto resolved = ResolveEbootPath(eboot_path);
    if (!resolved.has_value()) {
        LOG_ERROR(Loader, "shadps4_prepare_window: game ID or file path not found: {}",
                  eboot_path);
        return -1;
    }

    Core::Loader::Elf executable;
    executable.Open(*resolved);
    if (!executable.IsElfFile()) {
        LOG_ERROR(Loader, "shadps4_prepare_window: invalid PS4 executable: {}",
                  resolved->string());
        return -1;
    }

    auto* emulator = Common::Singleton<Core::Emulator>::Instance();
    emulator->executableName = "AetherPS4";
    emulator->PrepareWindow(*resolved);
    return 0;
}

extern "C" int shadps4_run_loop(void) {
    if (!g_init_ok) {
        return -1;
    }
    auto* emulator = Common::Singleton<Core::Emulator>::Instance();
    // RunLoop() blocks until the SDL window closes or shadps4_stop() pushes a quit event
    // (see emulator.cpp); SHADPS4_LIBRARY_BUILD makes it return here instead of
    // quick_exit()-ing the host process.
    emulator->RunLoop();
    return 0;
}

extern "C" int shadps4_run(const char* eboot_path) {
    if (shadps4_prepare_window(eboot_path) != 0) {
        return -1;
    }
    return shadps4_run_loop();
}

extern "C" void shadps4_stop() {
    if (!g_init_ok) {
        return;
    }
    Common::Singleton<Core::Emulator>::Instance()->Stop();
}

extern "C" void shadps4_toggle_pause() {
    if (!g_init_ok) {
        return;
    }
    Common::Singleton<Core::Emulator>::Instance()->TogglePause();
}

extern "C" int shadps4_is_paused() {
    if (!g_init_ok) {
        return 0;
    }
    return Common::Singleton<Core::Emulator>::Instance()->IsPaused() ? 1 : 0;
}

extern "C" int shadps4_has_presented_frame() {
    return Common::FramePresentedFlag().load(std::memory_order_relaxed) ? 1 : 0;
}

extern "C" void shadps4_register_first_frame_callback(void (*callback)(void)) {
    Common::FramePresentedCallback().store(callback, std::memory_order_relaxed);
}

extern "C" int shadps4_probe_jit() {
    // A page's worth is the smallest size the underlying protocol handles cleanly;
    // this is purely a functional probe, the region is released immediately either way.
    auto region = Core::DualMappedRegion::Allocate(4096);
    const bool ok = region.IsValid();
    region.Release();
    return ok ? 1 : 0;
}

extern "C" void* shadps4_get_uikit_window() {
    if (!g_init_ok) {
        return nullptr;
    }
    auto* emulator = Common::Singleton<Core::Emulator>::Instance();
    auto* window = emulator->GetWindow();
    if (window == nullptr) {
        return nullptr;
    }
    auto* sdl_window = window->GetSDLWindow();
    if (sdl_window == nullptr) {
        return nullptr;
    }
    return SDL_GetPointerProperty(SDL_GetWindowProperties(sdl_window),
                                  SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr);
}

extern "C" void shadps4_apply_touch_input(uint32_t buttons, int left_x, int left_y, int right_x,
                                          int right_y, int l2, int r2) {
    if (!g_init_ok) {
        return;
    }
    // Same primary-controller slot a real physical controller occupies (player_index - 1
    // for the default/only user), not a separate "extra" one -- see
    // touch_controls_layer.cpp's own kTouchControllerSlot comment for why: slot 4 is
    // reserved for ORBIS_PAD_PORT_TYPE_REMOTE_CONTROL-type pads specifically, and using it
    // meant every touch was updating a GameController the game was never looking at.
    constexpr size_t kTouchControllerSlot = 0;
    auto* controllers = Common::Singleton<Input::GameControllers>::Instance();
    const std::array<int, 6> axes = {left_x, left_y, right_x, right_y, l2, r2};
    (*controllers)[kTouchControllerSlot]->ApplyRemoteState(
        static_cast<Libraries::Pad::OrbisPadButtonDataOffset>(buttons), axes, false, 0.0f, 0.0f);
}
