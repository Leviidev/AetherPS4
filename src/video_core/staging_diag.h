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
 * Product defaults (Android/Vortek path — playable, no freeze):
 *   tick_lag=12       — free staging after N ticks (anti freeflight, no waitIdle thrash)
 *   strict_scratch=0  — lag free (strict+Wait+waitIdle froze gameplay)
 *   FHD always multi-slot ring in ObtainBufferForImage (independent of props)
 * Env / Android props can still force 0 or 1:
 *   BACHATA_STAGING_STRICT_SCRATCH / debug.bachata.staging_strict_scratch
 *   BACHATA_STAGING_STRICT_STREAM  / debug.bachata.staging_strict_stream
 *   BACHATA_STAGING_STRICT_BUFFER_CACHE=1  Mode F (opt-in): wait prior detile-read rewrite
 *   BACHATA_STAGING_TICK_LAG=N             Mode E diag lag
 *   BACHATA_BUFFER_CACHE_TICK_LAG=N        dig-only FHD lag (0=off)
 *
 * Host safety net (independent): wait_on_suballoc_overlap + suballoc_range_pool
 * default ON in vortek_gpu_track (R1/R2 proof).
 */
struct StagingDiagConfig {
    // Product (Mali/Vortek playable): lag free, not waitIdle thrash.
    // strict_scratch=1 + waitIdle froze Sonic (~13k DEVICE_IDLE / 180s).
    bool strict_scratch{false};
    bool strict_stream{true};
    bool strict_buffer_cache{false};
    u32 tick_lag{12}; // free after 12 ticks — longer multi-buffer (playable)
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
        StagingDiagConfig c{}; // product defaults: strict_scratch/stream = true
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

inline void LogStagingDiagConfigOnce() {
    auto& c = StagingDiag();
    if (c.config_logged) {
        return;
    }
    c.config_logged = true;
    LOG_WARNING(Render_Vulkan,
                "STAGING_DIAG_CONFIG strictScratch={} strictStream={} strictBufferCache={} "
                "tickLag={} bufferCacheTickLag={} path=guest_staging_ab",
                c.strict_scratch ? 1 : 0, c.strict_stream ? 1 : 0,
                c.strict_buffer_cache ? 1 : 0, c.tick_lag, c.buffer_cache_tick_lag);
}

/// FHD-class size threshold for provenance + strict stream ring (1920×1088×4 = 0x7f8000).
inline constexpr u32 kFullResStagingBytes = 0x700000;

inline bool IsFullResStagingSize(u32 size) {
    return size >= kFullResStagingBytes;
}

} // namespace VideoCore
