// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string_view>
#include <thread>

#include "common/singleton.h"
#include "core/linker.h"
#include "input/controller.h"
#include "sdl_window.h"

namespace Core {

using HLEInitDef = void (*)(Core::Loader::SymbolsResolver* sym);

struct SysModules {
    std::string_view module_name;
    HLEInitDef callback;
};

class Emulator {
public:
    Emulator();
    ~Emulator();

    void Run(std::filesystem::path file, std::vector<std::string> args = {},
             std::optional<std::filesystem::path> game_folder = {});
    void UpdatePlayTime(const std::string& serial);
    void Shutdown();

    /**
     * Requests that Run()'s event loop exit and return control to the caller. Safe to call
     * from any thread. On the CLI executable this behaves like closing the window; on the
     * embeddable library API (see src/platform/ios/shadps4_ios_api.cpp) this is what lets
     * shadps4_run() return instead of the process quick_exit()-ing.
     */
    void Stop();

    /// Toggles guest-thread pause via DebugState. Safe to call from any thread.
    void TogglePause();
    bool IsPaused() const;

    /// Returns the SDL window for the currently-running game, or nullptr before Run() has
    /// created one / after it has torn one down. See shadps4_ios_api.cpp's
    /// shadps4_get_uikit_window() for why a host UI needs this.
    Frontend::WindowSDL* GetWindow() const {
        return window.get();
    }

    /**
     * This will kill the current process and launch a new process with the same configuration
     * (using CLI args) but replacing the eboot image and guest arguments
     */
    void Restart(std::filesystem::path eboot_path, const std::vector<std::string>& guest_args = {});

    const char* executableName;
    bool waitForDebuggerBeforeRun{false};
    std::function<void()> onRuntimeRunning;
    std::function<void(std::string_view)> onRuntimeError;
    std::function<void(int)> onRuntimeStopped;

private:
    void LoadSystemModules(const std::string& game_serial);

    Core::MemoryManager* memory;
    Input::GameControllers* controllers;
    Core::Linker* linker;
    std::unique_ptr<Frontend::WindowSDL> window;
    std::chrono::steady_clock::time_point start_time;
    std::jthread play_time_thread;
};

} // namespace Core
