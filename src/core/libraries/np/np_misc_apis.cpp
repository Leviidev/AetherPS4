// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Three small, low-stakes PSN utility APIs: libSceNpLookup (async web-API request
// lifecycle), libSceNpWordFilter (chat comment sanitization), libSceNpBandwidthTest
// (network speed diagnostic). None of these have real Sony servers to talk to; each is
// implemented as the most honest safe behavior available without one:
//   - Lookup: requests complete immediately with no real network round-trip (same
//     "no async queue, complete inline" approach as libSceFios2's Op* API).
//   - WordFilter: passes the input comment through unfiltered rather than pretending to
//     apply a profanity filter that doesn't exist.
//   - BandwidthTest: reports immediate completion with a fixed placeholder bandwidth
//     value, since there is no real test being run.

#include <cstring>
#include <mutex>
#include <unordered_map>

#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/libs.h"
#include "core/libraries/np/np_error.h"

namespace Libraries::Np::MiscApis {

namespace {
std::mutex g_handle_mutex;
s32 g_next_handle = 1;
std::unordered_map<s32, bool> g_live_handles;

s32 AllocateHandle() {
    std::scoped_lock lock{g_handle_mutex};
    const s32 handle = g_next_handle++;
    g_live_handles[handle] = true;
    return handle;
}

bool ReleaseHandle(s32 handle) {
    std::scoped_lock lock{g_handle_mutex};
    return g_live_handles.erase(handle) == 1;
}
} // namespace

// --- libSceNpLookup ---

s32 PS4_SYSV_ABI sceNpLookupCreateTitleCtx(s32 npTitleId, void* userdata) {
    LOG_INFO(Lib_NpCommon, "sceNpLookupCreateTitleCtx: called");
    return AllocateHandle();
}

s32 PS4_SYSV_ABI sceNpLookupDeleteTitleCtx(s32 titleCtxId) {
    LOG_INFO(Lib_NpCommon, "sceNpLookupDeleteTitleCtx: titleCtxId={}", titleCtxId);
    return ReleaseHandle(titleCtxId) ? ORBIS_OK : ORBIS_NP_ERROR_INVALID_ARGUMENT;
}

s32 PS4_SYSV_ABI sceNpLookupCreateAsyncRequest(s32 titleCtxId) {
    LOG_INFO(Lib_NpCommon, "sceNpLookupCreateAsyncRequest: titleCtxId={}", titleCtxId);
    return AllocateHandle();
}

s32 PS4_SYSV_ABI sceNpLookupDeleteRequest(s32 reqId) {
    LOG_INFO(Lib_NpCommon, "sceNpLookupDeleteRequest: reqId={}", reqId);
    return ReleaseHandle(reqId) ? ORBIS_OK : ORBIS_NP_ERROR_INVALID_ARGUMENT;
}

s32 PS4_SYSV_ABI sceNpLookupPollAsync(s32 reqId, s32* result) {
    // Every request "completes" the instant it's created (no real network round-trip
    // happens), so a poll immediately after CreateAsyncRequest always reports done.
    LOG_INFO(Lib_NpCommon, "sceNpLookupPollAsync: reqId={}", reqId);
    if (result != nullptr) {
        *result = 0;
    }
    return 1; // done
}

s32 PS4_SYSV_ABI sceNpLookupWaitAsync(s32 reqId, s32* result) {
    LOG_INFO(Lib_NpCommon, "sceNpLookupWaitAsync: reqId={}", reqId);
    if (result != nullptr) {
        *result = 0;
    }
    return ORBIS_OK;
}

// --- libSceNpWordFilter ---

s32 PS4_SYSV_ABI sceNpWordFilterCreateTitleCtx(s32 npTitleId, void* userdata) {
    LOG_INFO(Lib_NpCommon, "sceNpWordFilterCreateTitleCtx: called");
    return AllocateHandle();
}

s32 PS4_SYSV_ABI sceNpWordFilterDeleteTitleCtx(s32 titleCtxId) {
    LOG_INFO(Lib_NpCommon, "sceNpWordFilterDeleteTitleCtx: titleCtxId={}", titleCtxId);
    return ReleaseHandle(titleCtxId) ? ORBIS_OK : ORBIS_NP_ERROR_INVALID_ARGUMENT;
}

s32 PS4_SYSV_ABI sceNpWordFilterCreateRequest(s32 titleCtxId) {
    LOG_INFO(Lib_NpCommon, "sceNpWordFilterCreateRequest: titleCtxId={}", titleCtxId);
    return AllocateHandle();
}

s32 PS4_SYSV_ABI sceNpWordFilterDeleteRequest(s32 reqId) {
    LOG_INFO(Lib_NpCommon, "sceNpWordFilterDeleteRequest: reqId={}", reqId);
    return ReleaseHandle(reqId) ? ORBIS_OK : ORBIS_NP_ERROR_INVALID_ARGUMENT;
}

s32 PS4_SYSV_ABI sceNpWordFilterSanitizeComment(s32 reqId, const char* comment,
                                                char* sanitized_comment) {
    LOG_INFO(Lib_NpCommon, "sceNpWordFilterSanitizeComment: reqId={}", reqId);
    if (comment == nullptr || sanitized_comment == nullptr) {
        return ORBIS_NP_ERROR_INVALID_ARGUMENT;
    }
    std::strcpy(sanitized_comment, comment);
    return ORBIS_OK;
}

// --- libSceNpBandwidthTest ---

namespace {
bool g_bandwidth_test_running = false;
}

s32 PS4_SYSV_ABI sceNpBandwidthTestInitStart(s32 threadPriority, u64 cpuAffinityMask,
                                             s32 threadStackSize) {
    LOG_INFO(Lib_NpCommon, "sceNpBandwidthTestInitStart: called");
    g_bandwidth_test_running = true;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpBandwidthTestGetStatus(s32* result, double* upBps, double* downBps) {
    LOG_INFO(Lib_NpCommon, "sceNpBandwidthTestGetStatus: called");
    // Reports the test as already finished (no real measurement is performed) with a
    // generic broadband-class placeholder figure rather than 0, which some games treat
    // as "test failed" and refuse to proceed past a bandwidth gate.
    if (result != nullptr) {
        *result = 0; // ORBIS_NP_BANDWIDTH_TEST_RESULT_SUCCESS-equivalent
    }
    if (upBps != nullptr) {
        *upBps = 10.0 * 1000.0 * 1000.0;
    }
    if (downBps != nullptr) {
        *downBps = 50.0 * 1000.0 * 1000.0;
    }
    g_bandwidth_test_running = false;
    return 2; // ORBIS_NP_BANDWIDTH_TEST_STATUS_FINISHED-equivalent
}

s32 PS4_SYSV_ABI sceNpBandwidthTestAbort() {
    LOG_INFO(Lib_NpCommon, "sceNpBandwidthTestAbort: called");
    g_bandwidth_test_running = false;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpBandwidthTestShutdown() {
    LOG_INFO(Lib_NpCommon, "sceNpBandwidthTestShutdown: called");
    g_bandwidth_test_running = false;
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("8533Q+LU7EQ", "libSceNpLookup", 1, "libSceNpLookup", sceNpLookupCreateTitleCtx);
    LIB_FUNCTION("mtqDK9zkoIE", "libSceNpLookup", 1, "libSceNpLookup", sceNpLookupDeleteTitleCtx);
    LIB_FUNCTION("JA4+sS39GMs", "libSceNpLookup", 1, "libSceNpLookup",
                 sceNpLookupCreateAsyncRequest);
    LIB_FUNCTION("wLaxchvEEnk", "libSceNpLookup", 1, "libSceNpLookup", sceNpLookupDeleteRequest);
    LIB_FUNCTION("V4EVrruHuy8", "libSceNpLookup", 1, "libSceNpLookup", sceNpLookupPollAsync);
    LIB_FUNCTION("YX9dAus6baE", "libSceNpLookup", 1, "libSceNpLookup", sceNpLookupWaitAsync);

    LIB_FUNCTION("r9BgI0PfJZg", "libSceNpWordFilter", 1, "libSceNpWordFilter",
                 sceNpWordFilterCreateTitleCtx);
    LIB_FUNCTION("t0P5z5yuFPA", "libSceNpWordFilter", 1, "libSceNpWordFilter",
                 sceNpWordFilterDeleteTitleCtx);
    LIB_FUNCTION("iCq5xW5KQW4", "libSceNpWordFilter", 1, "libSceNpWordFilter",
                 sceNpWordFilterCreateRequest);
    LIB_FUNCTION("PYFS1H70bDs", "libSceNpWordFilter", 1, "libSceNpWordFilter",
                 sceNpWordFilterDeleteRequest);
    LIB_FUNCTION("Jj4mkpFO2gE", "libSceNpWordFilter", 1, "libSceNpWordFilter",
                 sceNpWordFilterSanitizeComment);

    LIB_FUNCTION("jktww3yJXnc", "libSceNpBandwidthTest", 1, "libSceNpBandwidthTest",
                 sceNpBandwidthTestInitStart);
    LIB_FUNCTION("BYIZGKm6bO4", "libSceNpBandwidthTest", 1, "libSceNpBandwidthTest",
                 sceNpBandwidthTestGetStatus);
    LIB_FUNCTION("kvdMF48mB3Y", "libSceNpBandwidthTest", 1, "libSceNpBandwidthTest",
                 sceNpBandwidthTestAbort);
    LIB_FUNCTION("pLr1fEQS1z8", "libSceNpBandwidthTest", 1, "libSceNpBandwidthTest",
                 sceNpBandwidthTestShutdown);
}

} // namespace Libraries::Np::MiscApis
