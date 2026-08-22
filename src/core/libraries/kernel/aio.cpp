// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <mutex>
#include <thread>
#include <vector>

#include "aio.h"
#include "common/assert.h"
#include "common/debug.h"
#include "common/logging/log.h"
#include "core/libraries/kernel/equeue.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/libs.h"
#include "file_system.h"

namespace Libraries::Kernel {

constexpr s32 MAX_QUEUE = 512;

static std::mutex g_aio_mutex;
static std::vector<s32> g_id_state(MAX_QUEUE, 0);
static s32 g_id_index = 1;

s32 PS4_SYSV_ABI sceKernelAioInitializeImpl(void* p, s32 size) {
    return 0;
}

s32 PS4_SYSV_ABI sceKernelAioDeleteRequest(OrbisKernelAioSubmitId id, s32* ret) {
    if (ret == nullptr) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }
    std::lock_guard lock{g_aio_mutex};
    if (id >= 0 && id < MAX_QUEUE) {
        g_id_state[id] = ORBIS_KERNEL_AIO_STATE_ABORTED;
    }
    *ret = 0;
    return 0;
}

s32 PS4_SYSV_ABI sceKernelAioDeleteRequests(OrbisKernelAioSubmitId id[], s32 num, s32 ret[]) {
    if (ret == nullptr || id == nullptr) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }
    std::lock_guard lock{g_aio_mutex};
    for (s32 i = 0; i < num; i++) {
        if (id[i] >= 0 && id[i] < MAX_QUEUE) {
            g_id_state[id[i]] = ORBIS_KERNEL_AIO_STATE_ABORTED;
        }
        ret[i] = 0;
    }
    return 0;
}

s32 PS4_SYSV_ABI sceKernelAioPollRequest(OrbisKernelAioSubmitId id, s32* state) {
    if (state == nullptr) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }
    std::lock_guard lock{g_aio_mutex};
    if (id >= 0 && id < MAX_QUEUE) {
        *state = g_id_state[id];
    } else {
        *state = ORBIS_KERNEL_AIO_STATE_ABORTED;
    }
    return 0;
}

s32 PS4_SYSV_ABI sceKernelAioPollRequests(OrbisKernelAioSubmitId id[], s32 num, s32 state[]) {
    if (state == nullptr || id == nullptr) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }
    std::lock_guard lock{g_aio_mutex};
    for (s32 i = 0; i < num; i++) {
        if (id[i] >= 0 && id[i] < MAX_QUEUE) {
            state[i] = g_id_state[id[i]];
        } else {
            state[i] = ORBIS_KERNEL_AIO_STATE_ABORTED;
        }
    }
    return 0;
}

s32 PS4_SYSV_ABI sceKernelAioCancelRequest(OrbisKernelAioSubmitId id, s32* state) {
    if (state == nullptr) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }
    std::lock_guard lock{g_aio_mutex};
    if (id > 0 && id < MAX_QUEUE) {
        g_id_state[id] = ORBIS_KERNEL_AIO_STATE_ABORTED;
        *state = ORBIS_KERNEL_AIO_STATE_ABORTED;
    } else {
        *state = ORBIS_KERNEL_AIO_STATE_PROCESSING;
    }
    return 0;
}

s32 PS4_SYSV_ABI sceKernelAioCancelRequests(OrbisKernelAioSubmitId id[], s32 num, s32 state[]) {
    if (state == nullptr || id == nullptr) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }
    std::lock_guard lock{g_aio_mutex};
    for (s32 i = 0; i < num; i++) {
        if (id[i] > 0 && id[i] < MAX_QUEUE) {
            g_id_state[id[i]] = ORBIS_KERNEL_AIO_STATE_ABORTED;
            state[i] = ORBIS_KERNEL_AIO_STATE_ABORTED;
        } else {
            state[i] = ORBIS_KERNEL_AIO_STATE_PROCESSING;
        }
    }
    return 0;
}

s32 PS4_SYSV_ABI sceKernelAioWaitRequest(OrbisKernelAioSubmitId id, s32* state, u32* usec) {
    if (state == nullptr) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }
    u32 timer = 0;
    s32 timeout = 0;

    while (true) {
        {
            std::lock_guard lock{g_aio_mutex};
            if (id < 0 || id >= MAX_QUEUE || g_id_state[id] != ORBIS_KERNEL_AIO_STATE_PROCESSING) {
                *state = (id >= 0 && id < MAX_QUEUE) ? g_id_state[id] : ORBIS_KERNEL_AIO_STATE_COMPLETED;
                break;
            }
        }
        sceKernelUsleep(10);
        timer += 10;
        if (usec && *usec > 0 && timer > *usec) {
            timeout = 1;
            break;
        }
    }

    if (timeout) {
        return ORBIS_KERNEL_ERROR_ETIMEDOUT;
    }
    return 0;
}

s32 PS4_SYSV_ABI sceKernelAioWaitRequests(OrbisKernelAioSubmitId id[], s32 num, s32 state[],
                                          u32 mode, u32* usec) {
    if (state == nullptr || id == nullptr) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }
    u32 timer = 0;
    s32 timeout = 0;
    s32 completion = 0;

    for (s32 i = 0; i < num; i++) {
        if (!completion && !timeout) {
            while (true) {
                {
                    std::lock_guard lock{g_aio_mutex};
                    const auto cur_id = id[i];
                    if (cur_id < 0 || cur_id >= MAX_QUEUE || g_id_state[cur_id] != ORBIS_KERNEL_AIO_STATE_PROCESSING) {
                        break;
                    }
                }
                sceKernelUsleep(10);
                timer += 10;
                if (usec && *usec > 0 && timer > *usec) {
                    timeout = 1;
                    break;
                }
            }
        }

        std::lock_guard lock{g_aio_mutex};
        const auto cur_id = id[i];
        const auto cur_state = (cur_id >= 0 && cur_id < MAX_QUEUE) ? g_id_state[cur_id] : ORBIS_KERNEL_AIO_STATE_COMPLETED;
        if (mode == 0x02 && cur_state == ORBIS_KERNEL_AIO_STATE_COMPLETED) {
            completion = 1;
        }
        state[i] = cur_state;
    }

    if (timeout) {
        return ORBIS_KERNEL_ERROR_ETIMEDOUT;
    }
    return 0;
}

s32 PS4_SYSV_ABI sceKernelAioSubmitReadCommands(OrbisKernelAioRWRequest req[], s32 size, s32 prio,
                                                OrbisKernelAioSubmitId* id) {
    if (req == nullptr || id == nullptr) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }

    s32 this_id = 0;
    {
        std::lock_guard lock{g_aio_mutex};
        this_id = g_id_index;
        g_id_state[this_id] = ORBIS_KERNEL_AIO_STATE_PROCESSING;
        g_id_index = (g_id_index + 1) % MAX_QUEUE;
        if (!g_id_index) g_id_index = 1;
    }

    for (s32 i = 0; i < size; i++) {
        s64 ret = sceKernelPread(req[i].fd, req[i].buf, req[i].nbyte, req[i].offset);
        if (req[i].result != nullptr) {
            if (ret < 0) {
                req[i].result->state = ORBIS_KERNEL_AIO_STATE_ABORTED;
                req[i].result->returnValue = ret;
            } else {
                req[i].result->state = ORBIS_KERNEL_AIO_STATE_COMPLETED;
                req[i].result->returnValue = ret;
            }
        }
    }

    {
        std::lock_guard lock{g_aio_mutex};
        g_id_state[this_id] = ORBIS_KERNEL_AIO_STATE_COMPLETED;
    }
    *id = this_id;
    return 0;
}

s32 PS4_SYSV_ABI sceKernelAioSubmitReadCommandsMultiple(OrbisKernelAioRWRequest req[], s32 size,
                                                        s32 prio, OrbisKernelAioSubmitId id[]) {
    if (req == nullptr || id == nullptr) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }
    for (s32 i = 0; i < size; i++) {
        s32 this_id = 0;
        {
            std::lock_guard lock{g_aio_mutex};
            this_id = g_id_index;
            g_id_state[this_id] = ORBIS_KERNEL_AIO_STATE_PROCESSING;
            g_id_index = (g_id_index + 1) % MAX_QUEUE;
            if (!g_id_index) g_id_index = 1;
        }

        s64 ret = sceKernelPread(req[i].fd, req[i].buf, req[i].nbyte, req[i].offset);
        if (req[i].result != nullptr) {
            if (ret < 0) {
                req[i].result->state = ORBIS_KERNEL_AIO_STATE_ABORTED;
                req[i].result->returnValue = ret;
            } else {
                req[i].result->state = ORBIS_KERNEL_AIO_STATE_COMPLETED;
                req[i].result->returnValue = ret;
            }
        }

        {
            std::lock_guard lock{g_aio_mutex};
            g_id_state[this_id] = (ret < 0) ? ORBIS_KERNEL_AIO_STATE_ABORTED : ORBIS_KERNEL_AIO_STATE_COMPLETED;
        }
        id[i] = this_id;
    }
    return 0;
}

s32 PS4_SYSV_ABI sceKernelAioSubmitWriteCommands(OrbisKernelAioRWRequest req[], s32 size, s32 prio,
                                                 OrbisKernelAioSubmitId* id) {
    if (req == nullptr || id == nullptr) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }

    s32 this_id = 0;
    {
        std::lock_guard lock{g_aio_mutex};
        this_id = g_id_index;
        g_id_state[this_id] = ORBIS_KERNEL_AIO_STATE_PROCESSING;
        g_id_index = (g_id_index + 1) % MAX_QUEUE;
        if (!g_id_index) g_id_index = 1;
    }

    for (s32 i = 0; i < size; i++) {
        s64 ret = sceKernelPwrite(req[i].fd, req[i].buf, req[i].nbyte, req[i].offset);
        if (req[i].result != nullptr) {
            if (ret < 0) {
                req[i].result->state = ORBIS_KERNEL_AIO_STATE_ABORTED;
                req[i].result->returnValue = ret;
            } else {
                req[i].result->state = ORBIS_KERNEL_AIO_STATE_COMPLETED;
                req[i].result->returnValue = ret;
            }
        }
    }

    {
        std::lock_guard lock{g_aio_mutex};
        g_id_state[this_id] = ORBIS_KERNEL_AIO_STATE_COMPLETED;
    }
    *id = this_id;
    return 0;
}

s32 PS4_SYSV_ABI sceKernelAioSubmitWriteCommandsMultiple(OrbisKernelAioRWRequest req[], s32 size,
                                                         s32 prio, OrbisKernelAioSubmitId id[]) {
    if (req == nullptr || id == nullptr) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }
    for (s32 i = 0; i < size; i++) {
        s32 this_id = 0;
        {
            std::lock_guard lock{g_aio_mutex};
            this_id = g_id_index;
            g_id_state[this_id] = ORBIS_KERNEL_AIO_STATE_PROCESSING;
            g_id_index = (g_id_index + 1) % MAX_QUEUE;
            if (!g_id_index) g_id_index = 1;
        }

        s64 ret = sceKernelPwrite(req[i].fd, req[i].buf, req[i].nbyte, req[i].offset);
        if (req[i].result != nullptr) {
            if (ret < 0) {
                req[i].result->state = ORBIS_KERNEL_AIO_STATE_ABORTED;
                req[i].result->returnValue = ret;
            } else {
                req[i].result->state = ORBIS_KERNEL_AIO_STATE_COMPLETED;
                req[i].result->returnValue = ret;
            }
        }

        {
            std::lock_guard lock{g_aio_mutex};
            g_id_state[this_id] = (ret < 0) ? ORBIS_KERNEL_AIO_STATE_ABORTED : ORBIS_KERNEL_AIO_STATE_COMPLETED;
        }
        id[i] = this_id;
    }
    return 0;
}

s32 PS4_SYSV_ABI sceKernelAioSetParam() {
    LOG_ERROR(Kernel, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelAioInitializeParam() {
    LOG_ERROR(Kernel, "(STUBBED) called");
    return ORBIS_OK;
}

void RegisterAio(Core::Loader::SymbolsResolver* sym) {
    {
        std::lock_guard lock{g_aio_mutex};
        g_id_index = 1;
        std::fill(g_id_state.begin(), g_id_state.end(), 0);
    }

    LIB_FUNCTION("fR521KIGgb8", "libkernel", 1, "libkernel", sceKernelAioCancelRequest);
    LIB_FUNCTION("3Lca1XBrQdY", "libkernel", 1, "libkernel", sceKernelAioCancelRequests);
    LIB_FUNCTION("5TgME6AYty4", "libkernel", 1, "libkernel", sceKernelAioDeleteRequest);
    LIB_FUNCTION("Ft3EtsZzAoY", "libkernel", 1, "libkernel", sceKernelAioDeleteRequests);
    LIB_FUNCTION("vYU8P9Td2Zo", "libkernel", 1, "libkernel", sceKernelAioInitializeImpl);
    LIB_FUNCTION("nu4a0-arQis", "libkernel", 1, "libkernel", sceKernelAioInitializeParam);
    LIB_FUNCTION("2pOuoWoCxdk", "libkernel", 1, "libkernel", sceKernelAioPollRequest);
    LIB_FUNCTION("o7O4z3jwKzo", "libkernel", 1, "libkernel", sceKernelAioPollRequests);
    LIB_FUNCTION("9WK-vhNXimw", "libkernel", 1, "libkernel", sceKernelAioSetParam);
    LIB_FUNCTION("HgX7+AORI58", "libkernel", 1, "libkernel", sceKernelAioSubmitReadCommands);
    LIB_FUNCTION("lXT0m3P-vs4", "libkernel", 1, "libkernel",
                 sceKernelAioSubmitReadCommandsMultiple);
    LIB_FUNCTION("XQ8C8y+de+E", "libkernel", 1, "libkernel", sceKernelAioSubmitWriteCommands);
    LIB_FUNCTION("xT3Cpz0yh6Y", "libkernel", 1, "libkernel",
                 sceKernelAioSubmitWriteCommandsMultiple);
    LIB_FUNCTION("KOF-oJbQVvc", "libkernel", 1, "libkernel", sceKernelAioWaitRequest);
    LIB_FUNCTION("lgK+oIWkJyA", "libkernel", 1, "libkernel", sceKernelAioWaitRequests);
}

} // namespace Libraries::Kernel