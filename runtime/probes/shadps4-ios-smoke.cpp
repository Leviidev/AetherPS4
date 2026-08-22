// SPDX-License-Identifier: MIT

// Phase 1 verification for the shadps4_ios embeddable library (see the AetherPS4 iOS
// port plan): confirms the C API in src/platform/ios/shadps4_ios_api.h actually links,
// initializes the engine, and drives a real eboot through shadps4_init/shadps4_run to
// the same "game running" state the shadps4 CLI executable reaches -- without the CLI
// executable's process ever existing. Not a JIT/FEXCore test (that's Phase 3); this
// only proves the API restructuring in Phase 1 didn't regress anything.

#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <thread>

#include "platform/ios/shadps4_ios_api.h"

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <path-to-eboot.bin-or-pkg-dir>\n", argv[0]);
        return 2;
    }
    const char* eboot_path = argv[1];

    ShadPS4Options options{};
    options.user_dir = nullptr; // use the default user directory
    options.show_fps = 1;
    options.fullscreen = 0;

    std::printf("SHADPS4_IOS_SMOKE:INIT_BEFORE\n");
    if (shadps4_init(&options) != 0) {
        std::fprintf(stderr, "SHADPS4_IOS_SMOKE_FAIL check=init\n");
        return 1;
    }
    std::printf("SHADPS4_IOS_SMOKE:INIT_AFTER\n");

    // shadps4_run() must run on the main thread (SDL's Cocoa/UIKit backend creates the
    // window here); the stop-after-a-few-seconds timer is what runs on a background
    // thread instead.
    std::thread stop_thread([&] {
        // Give the game enough time to load, link, and open its window before
        // requesting a clean stop -- this probe is checking that the whole pipeline
        // reaches "running", not that the game runs to completion.
        std::this_thread::sleep_for(std::chrono::seconds(8));
        std::printf("SHADPS4_IOS_SMOKE:STOP_BEFORE paused=%d\n", shadps4_is_paused());
        shadps4_stop();
    });

    std::printf("SHADPS4_IOS_SMOKE:RUN_BEFORE\n");
    const int run_result = shadps4_run(eboot_path);
    std::printf("SHADPS4_IOS_SMOKE:RUN_AFTER result=%d\n", run_result);

    stop_thread.join();

    if (run_result != 0) {
        std::fprintf(stderr, "SHADPS4_IOS_SMOKE_FAIL check=run result=%d\n", run_result);
        return 1;
    }

    std::printf("SHADPS4_IOS_SMOKE_OK\n");
    return 0;
}
