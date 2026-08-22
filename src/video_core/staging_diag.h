// SPDX-FileCopyrightText: Copyright 2026 Bachata-S4
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdlib>
#include <cstring>

#include "common/logging/log.h"
#include "common/types.h"

namespace VideoCore {

/**
 * Guest staging knobs for Mali/Vortek freeflight (Sonic CUSA07023 class).
 *
 * Product defaults (Turnip / Adreno — mainline path):
 *   mali_gpu_opt=0    — create+destroy detile scratch (main branch behaviour)
 *   dig logs off
 *   FHD upload ring only with BACHATA_STAGING_FHD_RING
 *
 * Mali optimizations (Android driver settings toggle → BACHATA_MALI_GPU_OPT=1):
 *   multi-slot detile scratch ring + tick lag free (anti freeflight / DEVICE_LOST)
 *   optional dig props still override strict/lag/FHD ring
 *
 * Env / Android props:
 *   BACHATA_MALI_GPU_OPT / debug.bachata.mali_gpu_opt
 *   BACHATA_STAGING_VERBOSE / debug.bachata.staging_verbose  (hot-path dig logs)
 *   BACHATA_STAGING_STRICT_SCRATCH / debug.bachata.staging_strict_scratch
 *   BACHATA_STAGING_STRICT_STREAM  / debug.bachata.staging_strict_stream
 *   BACHATA_STAGING_STRICT_BUFFER_CACHE=1  Mode F (opt-in)
 *   BACHATA_STAGING_TICK_LAG=N             Mode E lag (Mali opt sets 12 by default)
 *   BACHATA_BUFFER_CACHE_TICK_LAG=N        dig-only FHD lag (0=off)
 *   BACHATA_STAGING_FHD_RING=1             FHD multi-slot ObtainBufferForImage
 */
struct StagingDiagConfig {
    bool mali_gpu_opt{false};
    bool verbose{false};
    bool strict_scratch{false};
    bool strict_stream{false};
    bool strict_buffer_cache{false};
    u32 tick_lag{0}; // 0 = free when submitted && IsFree (no lag floor)
    u32 buffer_cache_tick_lag{0};
    bool config_logged{false};
};

inline bool StagingEnvTruthy(const char* value) {
    if (!value || value[0] == '\0' || value[0] == '0') {
        return false;
    }
    if (value[0] == 'f' || value[0] == 'F' || value[0] == 'n' || value[0] == 'N') {
        return false;
    }
    return true;
}

inline StagingDiagConfig& StagingDiag() {
    static StagingDiagConfig cfg = [] {
        StagingDiagConfig c{};
        if (const char* e = std::getenv("BACHATA_MALI_GPU_OPT")) {
            c.mali_gpu_opt = StagingEnvTruthy(e);
        }
        if (const char* e = std::getenv("BACHATA_STAGING_VERBOSE")) {
            c.verbose = StagingEnvTruthy(e);
        }
        if (const char* e = std::getenv("BACHATA_STAGING_STRICT_SCRATCH")) {
            c.strict_scratch = StagingEnvTruthy(e);
        }
        if (const char* e = std::getenv("BACHATA_STAGING_STRICT_STREAM")) {
            c.strict_stream = StagingEnvTruthy(e);
        }
        if (const char* e = std::getenv("BACHATA_STAGING_STRICT_BUFFER_CACHE")) {
            c.strict_buffer_cache = StagingEnvTruthy(e);
        }
        if (const char* e = std::getenv("BACHATA_STAGING_TICK_LAG")) {
            char* end = nullptr;
            const unsigned long v = std::strtoul(e, &end, 10);
            if (end != e && v <= 64) {
                c.tick_lag = static_cast<u32>(v);
            }
        }
        if (const char* e = std::getenv("BACHATA_BUFFER_CACHE_TICK_LAG")) {
            char* end = nullptr;
            const unsigned long v = std::strtoul(e, &end, 10);
            if (end != e && v <= 64) {
                c.buffer_cache_tick_lag = static_cast<u32>(v);
            }
        }
        return c;
    }();
    return cfg;
}

inline bool MaliGpuOptEnabled() {
    return StagingDiag().mali_gpu_opt;
}

inline bool StagingVerbose() {
    return StagingDiag().verbose;
}

inline void LogStagingDiagConfigOnce() {
    auto& c = StagingDiag();
    if (c.config_logged) {
        return;
    }
    c.config_logged = true;
    if (!c.mali_gpu_opt && !c.verbose) {
        return;
    }
    LOG_WARNING(Render_Vulkan,
                "STAGING_DIAG_CONFIG maliGpuOpt={} verbose={} strictScratch={} strictStream={} "
                "strictBufferCache={} tickLag={} bufferCacheTickLag={} path=guest_staging_ab",
                c.mali_gpu_opt ? 1 : 0, c.verbose ? 1 : 0, c.strict_scratch ? 1 : 0,
                c.strict_stream ? 1 : 0, c.strict_buffer_cache ? 1 : 0, c.tick_lag,
                c.buffer_cache_tick_lag);
}

/// FHD-class size threshold for provenance + strict stream ring (1920×1088×4 = 0x7f8000).
inline constexpr u32 kFullResStagingBytes = 0x700000;

inline bool IsFullResStagingSize(u32 size) {
    return size >= kFullResStagingBytes;
}

} // namespace VideoCore
