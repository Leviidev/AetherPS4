// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <bit>

#include "common/types.h"

namespace Shader {

struct ClipCullArrays {
    u32 clip_size{};
    u32 cull_size{};
};

/// Highest used clip/cull plane index + 1 from an 8-bit component mask.
[[nodiscard]] constexpr u32 AttributeUsedCount(u8 mask) noexcept {
    return mask == 0 ? 0u : static_cast<u32>(std::bit_width(static_cast<unsigned>(mask)));
}

/// Pick native ClipDistance / CullDistance array sizes that fit device limits.
/// Prefers keeping clip planes when combined clip+cull would overflow.
[[nodiscard]] constexpr ClipCullArrays SelectClipCullArrays(u32 used_clip, u32 used_cull,
                                                            u32 max_clip, u32 max_cull,
                                                            u32 max_combined, bool emit_native_clip,
                                                            bool emit_native_cull) noexcept {
    const u32 clip_cap = max_clip == 0 ? 8u : max_clip;
    const u32 cull_cap = max_cull == 0 ? 8u : max_cull;
    const u32 combined_cap = max_combined == 0 ? 8u : max_combined;

    ClipCullArrays out{};
    if (emit_native_clip && used_clip != 0) {
        out.clip_size = std::min({used_clip, clip_cap, combined_cap});
    }
    if (emit_native_cull && used_cull != 0) {
        const u32 remaining = combined_cap > out.clip_size ? combined_cap - out.clip_size : 0;
        out.cull_size = std::min({used_cull, cull_cap, remaining});
    }
    return out;
}

} // namespace Shader
