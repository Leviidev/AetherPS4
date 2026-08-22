// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "platform/bachata/ffmpeg_cpu_policy.h"

extern "C" {
#include <libavutil/cpu.h>
}

namespace Platform::Bachata {

ScopedBox64CompatibleFfmpegCpuPolicy::ScopedBox64CompatibleFfmpegCpuPolicy()
    : previous_cpu_flags(av_get_cpu_flags()) {
    av_force_cpu_flags(0);
}

ScopedBox64CompatibleFfmpegCpuPolicy::~ScopedBox64CompatibleFfmpegCpuPolicy() {
    av_force_cpu_flags(previous_cpu_flags);
}

} // namespace Platform::Bachata
