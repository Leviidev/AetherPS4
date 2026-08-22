// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Platform::Bachata {

class ScopedBox64CompatibleFfmpegCpuPolicy {
public:
    ScopedBox64CompatibleFfmpegCpuPolicy();
    ~ScopedBox64CompatibleFfmpegCpuPolicy();

    ScopedBox64CompatibleFfmpegCpuPolicy(const ScopedBox64CompatibleFfmpegCpuPolicy&) = delete;
    ScopedBox64CompatibleFfmpegCpuPolicy& operator=(
        const ScopedBox64CompatibleFfmpegCpuPolicy&) = delete;

private:
    int previous_cpu_flags;
};

} // namespace Platform::Bachata
