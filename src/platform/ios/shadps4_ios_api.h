// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <stdint.h>

// Plain C ABI for embedding shadps4 into a host app process (the standalone AetherPS4
// iOS app; potentially other Swift hosts later) instead of running it as the `shadps4`
// CLI executable. Built as the `shadps4_ios` CMake target (SHADPS4_LIBRARY_BUILD),
// which compiles the same emulator/core sources as the CLI target with src/main.cpp's
// CLI/process-lifetime responsibilities replaced by these entry points; see
// src/platform/ios/shadps4_ios_api.cpp for what each call actually does.
//
// The standalone app lets SDL3 own the window and its own input/event handling (see
// PORTING plan, Phase 1 context) exactly as it already does on the desktop build, so
// there is no separate input API here for v1 -- gamepad/touch input flows through SDL's
// normal event pump once shadps4_run() creates the window.

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ShadPS4Options {
    // Overrides the user data directory (settings, saves, logs, etc). Must be
    // writable. Pass the app's Application Support / Documents directory on iOS.
    const char* user_dir;
    int show_fps;       // 0/1
    int fullscreen;     // -1 = leave at whatever EmulatorSettings already has, 0 = false, 1 = true
    int network_enabled; // 0/1 - enable network (ShadNet + system network)
} ShadPS4Options;

// Performs one-time engine bring-up (IPC, EmulatorState, UserSettings, KeyManager,
// EmulatorSettingsImpl). Must be called exactly once, before shadps4_run(), from the
// app's main thread during launch. Returns 0 on success.
int shadps4_init(const ShadPS4Options* options);

// Resolves eboot_path (an absolute path, or a game ID to search EmulatorSettings'
// configured install directories) and runs the emulator, blocking until the game
// exits or shadps4_stop() is called. MUST be called from the app's main thread: SDL's
// Cocoa/UIKit video backend creates and owns the window/event loop for the duration of
// this call (matching the desktop build's Frontend::WindowSDL usage), and Apple's
// windowing frameworks require that on the main thread -- calling this from a
// background thread fails during SDL video subsystem init. Returns 0 on a normal exit.
int shadps4_run(const char* eboot_path);

// Requests that a currently-running shadps4_run() call (on the main thread) return.
// Safe to call from any other thread, e.g. from a UI "stop" button's handler while
// shadps4_run() blocks the main thread.
void shadps4_stop(void);

// Toggles guest-thread pause. Safe to call from any thread.
void shadps4_toggle_pause(void);

// Returns non-zero if guest threads are currently paused.
int shadps4_is_paused(void);

// Returns non-zero once the Vulkan presenter has successfully presented at least one
// real frame since shadps4_init() (never resets to 0 again, even across game restarts
// within the same process). Intended for a host UI's boot loading screen to know when
// to dismiss itself in favor of actual game video, rather than guessing with a timeout.
// Safe to call from any thread.
int shadps4_has_presented_frame(void);

// Registers a callback invoked exactly once, synchronously, the instant the Vulkan
// presenter completes its first real swapchain present -- on the presenter's own render
// thread, NOT necessarily the thread that registered it and NOT necessarily the main
// thread, so a Swift/UIKit host must still hop to the main thread itself before touching
// any UI from inside the callback. Pushed rather than polled: a host UI's boot loading
// screen can dismiss itself the moment this fires instead of sampling
// shadps4_has_presented_frame() on a timer. Must be called before shadps4_run(). Passing
// nullptr clears any previously registered callback.
void shadps4_register_first_frame_callback(void (*callback)(void));

// Probes whether the JIT-granting protocol actually works right now: allocates a
// minimal dual-mapped JIT region (see core/ios/ios_jit_allocator.h) and immediately
// releases it. That allocation only succeeds if an external debugger (StikDebug) is
// attached AND is actively running the JIT script to service the BRK-trap handshake --
// unlike just checking whether *some* debugger is attached, this exercises the real
// end-to-end path the emulator itself depends on for JIT memory. Independent of
// shadps4_init()/shadps4_run() -- safe (and intended) to call before either, e.g. from
// a host UI's one-time setup check at launch. Returns non-zero on success.
//
// CALLER MUST confirm a debugger is actually attached (e.g. Swift's checkDebugged(),
// which only inspects the process's own codesign/csops state and never touches the
// BRK path) before calling this. The BRK instruction this triggers is unconditional --
// with no debugger attached to service it, it raises a raw, unhandled SIGTRAP that
// terminates the process outright, not a graceful failure (confirmed on-device; see
// fex_guest_engine.cpp's "Do NOT detach the debugger here" comment for the same
// failure mode hit via a different call site). Calling this blind, before any
// attachment check, WILL crash the app when StikDebug isn't attached.
int shadps4_probe_jit(void);

// Returns the raw UIWindow* SDL created for the currently-running game (as an untyped
// pointer, so this header stays C/Objective-C-agnostic; bridge it back with
// `(__bridge UIWindow *)` or Swift's `Unmanaged<UIWindow>.fromOpaque(...).takeUnretainedValue()`
// on the caller side), or NULL if no game is running. SDL creates this window itself --
// it is a separate UIWindow from whatever hosts the app's own SwiftUI content, not a view
// inside it -- so a host UI that wants native controls visible over gameplay must add them
// as a subview of THIS window (or of its rootViewController's view), not of its own
// window, since a second independent UIWindow was already confirmed on-device not to
// repaint reliably once shadps4_run() takes the main thread's SDL run loop (see
// mobile_overlay.h's header comment for that history). Only meaningful once SDL's window
// actually exists -- call this from a shadps4_register_first_frame_callback() callback (or
// later), not before shadps4_run() starts. Must be called from the main thread, same as
// every other UIKit-touching call in this API.
void* shadps4_get_uikit_window(void);

#ifdef __cplusplus
}
#endif
