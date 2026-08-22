// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <vector>
#include <CLI/CLI.hpp>
#include <SDL3/SDL_messagebox.h>

#include "common/arch.h"
#include "common/key_manager.h"
#include "common/logging/log.h"
#include "common/memory_patcher.h"
#include "common/path_util.h"
#include "common/sigsys_trap.h"
#include "core/debugger.h"
#include "core/emulator_settings.h"
#include "core/emulator_state.h"
#include "core/file_sys/fs.h"
#include "core/ipc/ipc.h"
#include "core/loader/elf.h"
#include "core/user_settings.h"
#include "common/singleton.h"
#include "emulator.h"
#include "input/controller.h"
#include "imgui/big_picture/big_picture.h"
#ifdef ENABLE_BACHATA_RUNTIME
#include "platform/bachata/runtime_client.h"
#endif

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <pthread.h>
#include <sys/sysctl.h>
#endif

int main(int argc, char* argv[]) {
    Common::InstallBachataSigsysTrap();
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

#if defined(__APPLE__) && defined(ARCH_X86_64)
    // KosmicKrisp only supports Apple Silicon. Check that we are not running on an Intel Mac.
    int sysctl_ret = 0;
    size_t sysctl_size = sizeof(sysctl_ret);
    sysctlbyname("sysctl.proc_translated", &sysctl_ret, &sysctl_size, nullptr, 0);
    if (sysctl_ret != 1) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "shadPS4",
                                 "shadPS4 only supports Apple Silicon Macs.", nullptr);
        std::cout << "shadPS4 only supports Apple Silicon Macs." << std::endl;
        return -1;
    }
#endif

    CLI::App app{"shadPS4 Emulator CLI"};

    // ---- CLI state ----
    std::optional<std::string> gamePath;
    std::vector<std::string> gameArgs;
    std::optional<std::filesystem::path> overrideRoot;
#ifdef ENABLE_BACHATA_RUNTIME
    std::optional<std::filesystem::path> bachataSocket;
    std::optional<std::filesystem::path> bachataStorageRoot;
#endif
    std::optional<int> waitPid;
    bool waitForDebugger = false;

    std::optional<std::string> fullscreenStr;
    bool ignoreGamePatch = false;
    bool showFps = false;
    bool configClean = false;
    bool configGlobal = false;
    bool bigPicture = false;

    std::optional<std::filesystem::path> addGameFolder;
    std::optional<std::filesystem::path> setAddonFolder;
    std::optional<std::string> patchFile;

    // ---- Options ----
    app.add_option("-g,--game", gamePath, "Game path or ID");
    app.add_option("-p,--patch", patchFile, "Patch file to apply");
    app.add_flag("-i,--ignore-game-patch", ignoreGamePatch,
                 "Disable automatic loading of game patches");

    app.add_flag("-b,--big-picture", bigPicture, "Start in Big Picture Mode");

    // FULLSCREEN: behavior-identical
    app.add_option("-f,--fullscreen", fullscreenStr, "Fullscreen mode (true|false)");

    app.add_option("--override-root", overrideRoot)->check(CLI::ExistingDirectory);
#ifdef ENABLE_BACHATA_RUNTIME
    app.add_option("--bachata-socket", bachataSocket, "Bachata Android runtime control socket");
    app.add_option("--bachata-storage-root", bachataStorageRoot,
                   "Bachata Android app-private storage root")
        ->check(CLI::ExistingDirectory);
#endif

    app.add_flag("--wait-for-debugger", waitForDebugger);
    app.add_option("--wait-for-pid", waitPid);

    app.add_flag("--show-fps", showFps);
    app.add_flag("--config-clean", configClean);
    app.add_flag("--config-global", configGlobal);
    app.add_flag("--log-append", Common::Log::g_should_append);

    app.add_option("--add-game-folder", addGameFolder)->check(CLI::ExistingDirectory);
    app.add_option("--set-addon-folder", setAddonFolder)->check(CLI::ExistingDirectory);

    // ---- Capture args after `--` verbatim ----
    app.allow_extras();
    app.parse_complete_callback([&]() {
        const auto& extras = app.remaining();
        if (!extras.empty()) {
            gameArgs = extras;
        }
    });

    // ---- No-args behavior ----
    if (argc == 1) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "shadPS4",
                                 "This is a CLI application. Please use the QTLauncher for a GUI:\n"
                                 "https://github.com/shadps4-emu/shadps4-qtlauncher/releases",
                                 nullptr);
        std::cout << app.help();
        return -1;
    }

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        return app.exit(e);
    }

#ifdef ENABLE_BACHATA_RUNTIME
    auto runtime_client = Platform::Bachata::RuntimeClient::Disabled();
    if (bachataSocket.has_value()) {
        if (!bachataStorageRoot.has_value() ||
            !Platform::Bachata::ValidateSocketPath(*bachataSocket, *bachataStorageRoot)) {
            std::cerr << "--bachata-socket must be absolute and inside --bachata-storage-root\n";
            return 1;
        }
        auto connected = Platform::Bachata::RuntimeClient::Connect(*bachataSocket);
        if (!connected.has_value() || !connected->SendHello()) {
            std::cerr << "Failed to connect Bachata runtime socket\n";
            return 1;
        }
        runtime_client = std::move(*connected);
        if (!runtime_client.SendStarting()) {
            std::cerr << "Failed to send Bachata Starting event\n";
            return 1;
        }
    }
#endif

#ifdef ENABLE_BACHATA_RUNTIME
    // Android always supplies an absolute eboot path. Reject malformed content before
    // IPC, settings, SDL, or X11 initialization can block the managed session.
    if (gamePath.has_value() && std::filesystem::path(*gamePath).is_absolute()) {
        const std::filesystem::path early_eboot_path(*gamePath);
        Core::Loader::Elf executable;
        executable.Open(early_eboot_path);
        if (!std::filesystem::exists(early_eboot_path) || !executable.IsElfFile()) {
            std::cerr << "Invalid PS4 executable: " << early_eboot_path << '\n';
            runtime_client.SendError("CONTENT_INVALID");
            runtime_client.SendStopped(1);
            return 1;
        }
    }
#endif

    if (waitPid)
        Core::Debugger::WaitForPid(*waitPid);

    // Initialize main log with default config
    Common::Log::Setup("shadps4.log");

    LOG_INFO(Debug, "Run: {}", std::span(argv, argc));

    IPC::Instance().Init();

    auto emu_state = std::make_shared<EmulatorState>();
    EmulatorState::SetInstance(emu_state);
    UserSettings.Load();
#ifdef ENABLE_BACHATA_RUNTIME
    // Desktop input discovery logs in player one during its first controller scan. The
    // Android runtime bypasses that scan, so bootstrap the same user-service login event.
    UserManagement.LoginUser(UserManagement.GetUserByPlayerIndex(1), 1);
    if (auto* user = UserManagement.GetUserByPlayerIndex(1)) {
        auto* controllers = Common::Singleton<Input::GameControllers>::Instance();
        (*controllers)[0]->user_id = user->user_id;
        (*controllers)[0]->ConnectController(nullptr);
    }
#endif

    // Initialize key manager
    auto key_manager = KeyManager::GetInstance();
    key_manager->LoadFromFile();

    // Load configurations
    std::shared_ptr<EmulatorSettingsImpl> emu_settings = std::make_shared<EmulatorSettingsImpl>();
    EmulatorSettingsImpl::SetInstance(emu_settings);
    emu_settings->Load();

    // Configure logger appropriately
    Common::Log::g_should_append |= EmulatorSettings.IsLogAppend();

    if (bigPicture) {
        BigPictureMode::Launch(argv[0]);
        return 0;
    }

    // ---- Utility commands ----
    if (addGameFolder) {
        EmulatorSettings.AddGameInstallDir(*addGameFolder);
        EmulatorSettings.Save();
        LOG_INFO(Config, "Game folder successfully saved.");
        return 0;
    }

    if (setAddonFolder) {
        EmulatorSettings.SetAddonInstallDir(*setAddonFolder);
        EmulatorSettings.Save();
        LOG_INFO(Config, "Addon folder successfully saved.");
        return 0;
    }

    if (!gamePath.has_value()) {
        if (!gameArgs.empty()) {
            gamePath = gameArgs.front();
            gameArgs.erase(gameArgs.begin());
        } else {
            LOG_ERROR(Debug, "Please provide a game path or ID.");
#ifdef ENABLE_BACHATA_RUNTIME
            runtime_client.SendError("CONTENT_INVALID");
            runtime_client.SendStopped(1);
#endif
            return 1;
        }
    }
    if (!gameArgs.empty()) {
        if (gameArgs.front() == "--") {
            gameArgs.erase(gameArgs.begin());
        } else {
            LOG_ERROR(Debug, "unhandled flags");
            return 1;
        }
    }

    // ---- Apply flags ----
    if (patchFile)
        MemoryPatcher::patch_file = *patchFile;

    if (ignoreGamePatch)
        Core::FileSys::MntPoints::ignore_game_patches = true;

    if (fullscreenStr) {
        if (*fullscreenStr == "true") {
            EmulatorSettings.SetFullScreen(true);
        } else if (*fullscreenStr == "false") {
            EmulatorSettings.SetFullScreen(false);
        } else {
            LOG_ERROR(Debug, "Invalid argument for --fullscreen (use true|false)");
            return 1;
        }
    }

    if (showFps)
        EmulatorSettings.SetShowFpsCounter(true);

    if (configClean)
        EmulatorSettings.SetConfigMode(ConfigMode::Clean);

    if (configGlobal)
        EmulatorSettings.SetConfigMode(ConfigMode::Global);

    // ---- Resolve game path or ID ----
    std::filesystem::path ebootPath(*gamePath);
    if (!std::filesystem::exists(ebootPath)) {
        bool found = false;
        constexpr int maxDepth = 5;
        for (const auto& installDir : EmulatorSettings.GetGameInstallDirs()) {
            if (auto foundPath = Common::FS::FindGameByID(installDir, *gamePath, maxDepth)) {
                ebootPath = *foundPath;
                found = true;
                break;
            }
        }
        if (!found) {
            LOG_ERROR(Debug, "Game ID or file path not found: {}", *gamePath);
#ifdef ENABLE_BACHATA_RUNTIME
            runtime_client.SendError("CONTENT_INVALID");
            runtime_client.SendStopped(1);
#endif
            return 1;
        }
    }

#ifdef ENABLE_BACHATA_RUNTIME
    Core::Loader::Elf executable;
    executable.Open(ebootPath);
    if (!executable.IsElfFile()) {
        LOG_ERROR(Debug, "Invalid PS4 executable: {}", ebootPath.string());
        runtime_client.SendError("CONTENT_INVALID");
        runtime_client.SendStopped(1);
        return 1;
    }
#endif

    auto* emulator = Common::Singleton<Core::Emulator>::Instance();
    emulator->executableName = argv[0];
    emulator->waitForDebuggerBeforeRun = waitForDebugger;
#ifdef ENABLE_BACHATA_RUNTIME
    if (runtime_client.IsEnabled() &&
        !runtime_client.StartInputReader([](const Platform::Bachata::ControllerSnapshot& snapshot) {
            auto* controllers = Common::Singleton<Input::GameControllers>::Instance();
            const std::array<int, 6> axes = {
                snapshot.left_x,       snapshot.left_y, snapshot.right_x,
                snapshot.right_y,      snapshot.left_trigger, snapshot.right_trigger,
            };
            (*controllers)[snapshot.slot]->ApplyRemoteState(
                static_cast<Libraries::Pad::OrbisPadButtonDataOffset>(snapshot.buttons), axes,
                snapshot.touch_down, snapshot.touch_x / 1920.0f, snapshot.touch_y / 1080.0f);
        })) {
        std::cerr << "Failed to start Bachata input reader\n";
        runtime_client.SendError("INPUT_UNAVAILABLE");
        runtime_client.SendStopped(1);
        return 1;
    }
    emulator->onRuntimeRunning = [&runtime_client]() { runtime_client.SendRunning(); };
    Platform::Bachata::SetActiveRuntimeClient(&runtime_client);
    emulator->onRuntimeError = [&runtime_client](std::string_view code) {
        runtime_client.SendError(code);
    };
    emulator->onRuntimeStopped = [&runtime_client](int exit_code) {
        runtime_client.SendStopped(exit_code);
    };
#endif
    emulator->Run(ebootPath, gameArgs, overrideRoot);

    return 0;
}
