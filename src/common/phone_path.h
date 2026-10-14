// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// AETHER_PHONE_PATH: the code paths the phone runs -- JIT memory as dual-mapped RW/RX pairs,
// the packed PS4 address-space layout, the batched HLE veneers -- as opposed to desktop
// shadPS4's. Defined on iOS, and on macOS when the build is the Mac harness for the phone
// (-DAETHER_MAC_PHONE_PATH=ON), so the code tested on the Mac is the code the phone runs.
// Things that are about StikDebug or iOS's sandbox alone stay under TARGET_OS_IPHONE.
#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE || defined(AETHER_MAC_PHONE_PATH)
#define AETHER_PHONE_PATH 1
#endif
#endif
