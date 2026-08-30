// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <boost/container/set.hpp>
#include <boost/container/small_vector.hpp>
#include "common/types.h"

namespace Serialization {
struct Archive;
}

namespace Shader {

using PFN_SrtWalker = void PS4_SYSV_ABI (*)(const u32* /*user_data*/, u32* /*flat_dst*/);
PFN_SrtWalker RegisterWalkerCode(const u8* ptr, size_t size);

// iOS only: forces the SRT walker JIT pool (flatten_extended_userdata_pass.cpp's
// IosSrtCodePool) to allocate now, on whatever thread calls this, rather than lazily on the
// first shader that actually needs a walker. Confirmed on-device that lazy initialization was
// too late: by the time the first shader needing a walker actually ran (well into gameplay),
// StikDebug had already stopped responding to JIT-mapping requests, and the pool's own
// fallback (a standalone per-walker request, the pre-pooling behavior) failed identically.
// Call this once, as early as possible -- before any guest code runs at all, alongside the
// same window/emulator setup that already needs the main thread -- so the one BreakGetJITMapping
// request this pool ever makes happens while StikDebug is reliably still attached. A no-op
// (and safe to call) on every other platform.
void WarmUpIosSrtCodePool();

struct PersistentSrtInfo {
    // Special case when fetch shader uses step rates.
    struct SrtSharpReservation {
        u32 sgpr_base;
        u32 dword_offset;
        u32 num_dwords;
    };

    PFN_SrtWalker walker_func{};
    size_t walker_func_size{};
    u32 flattened_bufsize_dw = 16; // NumUserDataRegs

    void Serialize(Serialization::Archive& ar) const;
    bool Deserialize(Serialization::Archive& ar);
};

} // namespace Shader
