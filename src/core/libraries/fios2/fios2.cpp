// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/logging/log.h"
#include "core/libraries/fios2/fios2.h"
#include "core/libraries/kernel/file_system.h"
#include "core/libraries/libs.h"
#include "common/singleton.h"
#include "core/file_sys/fs.h"
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
#include "core/guest_cpu/guest_callback.h"
#endif

namespace Libraries::Fios2 {

// The guest uses the async Fios2 op API (submit op -> OpWait -> OpGetActualCount).
// shadPS4 services I/O synchronously, so each async submitter runs the work inline and
// records the result under a fake op handle that the Op* queries read back. Fake handles
// start above zero so they never collide with the "invalid op" sentinel (-1 / 0).
//
// Some titles (CUSA00223) never call OpWait: they put a completion callback in
// SceFiosOpAttr and block on an event the callback signals. Real Fios fires that
// callback when the op completes; our inline path must do the same or the game hangs
// after FHOpen with no further Fios traffic.
namespace {
std::mutex g_op_mutex;
std::unordered_map<s32, s64> g_op_actual_counts;
s32 g_next_op_handle = 1;

// Vita/PS4 SceFiosOpAttr layout (64-bit). Confirmed against Vita SDK + OpenOrbis NIDs.
struct FiosOpAttr {
    s64 deadline;             // 0x00 absolute Fios time (us), 0 = none
    void* p_callback;         // 0x08 SceFiosOpCallback
    void* p_callback_context; // 0x10
    s32 priority_and_flags;   // 0x18 priority:8 | opflags:24
    u32 user_tag;             // 0x1c
    void* user_ptr;           // 0x20
    void* p_reserved;         // 0x28
};
static_assert(sizeof(FiosOpAttr) == 0x30, "FiosOpAttr size");

// typedef void (*SceFiosOpCallback)(void* pContext, s32 op, s32 err, s64 result);
using FiosOpCallback = void PS4_SYSV_ABI (*)(void* context, s32 op, s32 err, s64 result);

s32 AllocateOpHandle(s64 actual_count) {
    std::scoped_lock lock{g_op_mutex};
    const s32 op = g_next_op_handle++;
    g_op_actual_counts[op] = actual_count;
    return op;
}

s64 QueryOpActualCount(s32 op) {
    std::scoped_lock lock{g_op_mutex};
    const auto it = g_op_actual_counts.find(op);
    return it != g_op_actual_counts.end() ? it->second : 0;
}

void LogOpAttr(const char* where, const void* op_attr) {
    if (op_attr == nullptr) {
        LOG_INFO(Lib_SysModule, "[FIOS-HLE][{}] op_attr=null", where);
        return;
    }
    const auto* attr = static_cast<const FiosOpAttr*>(op_attr);
    LOG_INFO(Lib_SysModule,
             "[FIOS-HLE][{}] op_attr={} deadline={} cb={} ctx={} pri_flags={:#x} tag={:#x} "
             "user={} reserved={}",
             where, op_attr, attr->deadline, attr->p_callback, attr->p_callback_context,
             static_cast<u32>(attr->priority_and_flags), attr->user_tag, attr->user_ptr,
             attr->p_reserved);
}

// Fire SceFiosOpAttr completion callback after an inline-completed async op.
// err=0 and result=actual_count on success; err=negative / result=0 on failure.
void MaybeInvokeOpCallback(const void* op_attr, s32 op, s32 err, s64 result) {
    if (op_attr == nullptr) {
        return;
    }
    const auto* attr = static_cast<const FiosOpAttr*>(op_attr);
    if (attr->p_callback == nullptr) {
        return;
    }
    LOG_INFO(Lib_SysModule,
             "[FIOS-HLE][OpCallback] invoke cb={} ctx={} op={} err={} result={}",
             attr->p_callback, attr->p_callback_context, op, err, result);
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
    if (AetherPS4::GuestCpu::IsGuestFunctionAddress(attr->p_callback)) {
        AetherPS4::GuestCpu::RunGuestFunctionOrAbort(attr->p_callback, "sceFiosOpCallback",
                                                attr->p_callback_context, op, err, result);
        return;
    }
#endif
    // Native / non-FEX: call directly (host pointer or desktop path).
    auto* cb = reinterpret_cast<FiosOpCallback>(attr->p_callback);
    cb(attr->p_callback_context, op, err, result);
}

// Allocate op handle and complete via OpWait path AND optional callback.
s32 CompleteAsyncOp(const void* op_attr, s64 actual_count) {
    if (actual_count < 0) {
        const s32 err = static_cast<s32>(actual_count);
        MaybeInvokeOpCallback(op_attr, err, err, 0);
        return err;
    }
    const s32 op = AllocateOpHandle(actual_count);
    MaybeInvokeOpCallback(op_attr, op, 0, actual_count);
    return op;
}
} // namespace

struct FiosStat {
    s64 size;
    u64 access_date;
    u64 modification_date;
    u64 creation_date;
    u32 flags;
    u32 reserved;
    s64 uid;
    s64 gid;
    s64 dev;
    s64 ino;
    s64 mode;
};

s32 PS4_SYSV_ABI sceFiosInitialize(const void* parameters) {
    LOG_INFO(Lib_SysModule, "[FIOS-HLE][Initialize] parameters={}", parameters);
    return 0;
}

s32 PS4_SYSV_ABI sceFiosFHOpenSync(const void* op_attr, s32* handle, const char* path,
                                   const void* open_params) {
    if (handle == nullptr || path == nullptr) {
        return -1;
    }
    const s32 fd = Kernel::posix_open(path, 0, 0);
    if (fd < 0) {
        LOG_ERROR(Lib_SysModule, "[FIOS-HLE][FHOpenSync] failed path='{}'", path);
        return fd;
    }
    *handle = fd;
    LOG_INFO(Lib_SysModule, "[FIOS-HLE][FHOpenSync] path='{}' handle={} op_attr={} open_params={}",
             path, fd, op_attr, open_params);
    return 0;
}

s32 PS4_SYSV_ABI sceFiosFHOpenWithModeSync(const void* op_attr, s32* handle, const char* path,
                                           const void* open_params, u16 mode) {
    LOG_INFO(Lib_SysModule, "[FIOS-HLE][FHOpenWithModeSync] path='{}' mode={:#o}", path, mode);
    return sceFiosFHOpenSync(op_attr, handle, path, open_params);
}

s64 PS4_SYSV_ABI sceFiosFHReadSync(const void* op_attr, s32 handle, void* buffer, s64 size) {
    const s64 read = Kernel::sceKernelRead(handle, buffer, static_cast<u64>(size));
    LOG_DEBUG(Lib_SysModule, "[FIOS-HLE][FHReadSync] handle={} size={} read={} buffer={} op_attr={}",
              handle, size, read, buffer, op_attr);
    return read;
}

s64 PS4_SYSV_ABI sceFiosFHPreadSync(const void* op_attr, s32 handle, void* buffer, s64 size,
                                    s64 offset) {
    const s64 read = Kernel::sceKernelPread(handle, buffer, static_cast<u64>(size), offset);
    LOG_DEBUG(Lib_SysModule,
              "[FIOS-HLE][FHPreadSync] handle={} offset={:#x} size={} read={} buffer={} op_attr={}",
              handle, offset, size, read, buffer, op_attr);
    return read;
}

s32 PS4_SYSV_ABI sceFiosFHStatSync(const void* op_attr, s32 handle, FiosStat* stat) {
    if (stat == nullptr) {
        return -1;
    }
    Kernel::OrbisKernelStat kernel_stat{};
    const s32 result = Kernel::sceKernelFstat(handle, &kernel_stat);
    if (result >= 0) {
        *stat = {};
        stat->size = kernel_stat.st_size;
        stat->uid = kernel_stat.st_uid;
        stat->gid = kernel_stat.st_gid;
        stat->dev = kernel_stat.st_dev;
        stat->ino = kernel_stat.st_ino;
        stat->mode = kernel_stat.st_mode;
    }
    LOG_DEBUG(Lib_SysModule, "[FIOS-HLE][FHStatSync] handle={} size={} result={} op_attr={}",
              handle, result >= 0 ? stat->size : -1, result, op_attr);
    return result;
}

s64 PS4_SYSV_ABI sceFiosFHGetSize(s32 handle) {
    Kernel::OrbisKernelStat stat{};
    const s32 result = Kernel::sceKernelFstat(handle, &stat);
    const s64 size = result >= 0 ? stat.st_size : result;
    LOG_DEBUG(Lib_SysModule, "[FIOS-HLE][FHGetSize] handle={} size={}", handle, size);
    return size;
}

s64 PS4_SYSV_ABI sceFiosFHSeek(s32 handle, s64 offset, s32 whence) {
    const s64 result = Kernel::posix_lseek(handle, offset, whence);
    LOG_DEBUG(Lib_SysModule, "[FIOS-HLE][FHSeek] handle={} offset={} whence={} result={}", handle,
              offset, whence, result);
    return result;
}

s32 PS4_SYSV_ABI sceFiosIsValidHandle(s32 handle) {
    Kernel::OrbisKernelStat stat{};
    const bool valid = Kernel::sceKernelFstat(handle, &stat) >= 0;
    LOG_DEBUG(Lib_SysModule, "[FIOS-HLE][IsValidHandle] handle={} valid={}", handle, valid);
    return valid ? 1 : 0;
}

s32 PS4_SYSV_ABI sceFiosFHCloseSync(const void* op_attr, s32 handle) {
    const s32 result = Kernel::posix_close(handle);
    LOG_INFO(Lib_SysModule, "[FIOS-HLE][FHCloseSync] handle={} result={} op_attr={}", handle,
             result, op_attr);
    return result;
}


s32 PS4_SYSV_ABI sceFiosIOFilterAdd(s32 index, void* filter, void* context) {
    LOG_INFO(Lib_SysModule, "[FIOS-HLE][IOFilterAdd] index={} filter={} context={}", index, filter,
             context);
    return 0;
}

void* PS4_SYSV_ABI sceFiosIOFilterPsarcDearchiver() {
    // Unity often stores the dearchiver function pointer; a non-null dummy is enough
    // for registration. Actual decompression is not implemented here.
    static int dummy_psarc_filter = 1;
    LOG_INFO(Lib_SysModule, "[FIOS-HLE][IOFilterPsarcDearchiver] stub");
    return &dummy_psarc_filter;
}

s64 PS4_SYSV_ABI sceFiosArchiveGetMountBufferSizeSync(const void* op_attr, const char* path,
                                                      const void* params) {
    LOG_INFO(Lib_SysModule, "[FIOS-HLE][ArchiveGetMountBufferSizeSync] path='{}' op={} params={}",
             path ? path : "(null)", op_attr, params);
    // Generous buffer; real PSARC mount needs more work if game depends on archives.
    return 0x100000; // 1 MiB
}

s32 PS4_SYSV_ABI sceFiosArchiveMountSync(const void* op_attr, s32* handle, const char* path,
                                         const char* mount_point, void* mount_buffer,
                                         s64 mount_buffer_size, s32 flags) {
    static_cast<void>(op_attr);
    static_cast<void>(mount_buffer);
    static_cast<void>(mount_buffer_size);
    static_cast<void>(flags);
    auto* mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();
    // Extracted dumps often have no real .psarc; content lives under /app0 (game root).
    // Map the FIOS mount point to the same host folder so asset paths resolve.
    const auto app0_host = mnt->GetHostPath("/app0");
    std::string guest_mount = "/archive/mount/point";
    if (mount_point != nullptr && mount_point[0] != 0) {
        guest_mount = mount_point;
    }
    if (!app0_host.empty()) {
        mnt->Mount(app0_host, guest_mount, true);
        LOG_INFO(Lib_SysModule,
                 "[FIOS-HLE][ArchiveMountSync] mapped '{}' -> host '{}' (path request='{}')",
                 guest_mount, app0_host.string(), path ? path : "(null)");
    } else {
        LOG_ERROR(Lib_SysModule, "[FIOS-HLE][ArchiveMountSync] no /app0 host path for mount '{}'",
                  guest_mount);
    }
    if (handle) {
        *handle = 1; // non-zero fake handle; unmount not implemented
    }
    return 0;
}

s64 PS4_SYSV_ABI sceFiosFileGetSizeSync(const void* op_attr, const char* path) {
    if (path == nullptr) {
        return -1;
    }
    s32 handle = -1;
    const s32 open_result = sceFiosFHOpenSync(op_attr, &handle, path, nullptr);
    if (open_result < 0) {
        return open_result;
    }
    const s64 size = sceFiosFHGetSize(handle);
    sceFiosFHCloseSync(op_attr, handle);
    return size;
}

s32 PS4_SYSV_ABI sceFiosFileExistsSync(const void* op_attr, const char* path) {
    if (path == nullptr) {
        return 0;
    }
    s32 handle = -1;
    const s32 open_result = sceFiosFHOpenSync(op_attr, &handle, path, nullptr);
    if (open_result < 0) {
        return 0;
    }
    sceFiosFHCloseSync(op_attr, handle);
    return 1;
}

s32 PS4_SYSV_ABI sceFiosDirectoryExistsSync(const void* op_attr, const char* path) {
    LOG_INFO(Lib_SysModule, "[FIOS-HLE][DirectoryExistsSync] path='{}'", path ? path : "(null)");
    static_cast<void>(op_attr);
    // Best-effort: treat missing path as absent without full dir APIs.
    return path != nullptr ? 1 : 0;
}

s64 PS4_SYSV_ABI sceFiosFHTell(s32 handle) {
    return sceFiosFHSeek(handle, 0, 1 /*SEEK_CUR*/);
}

s64 PS4_SYSV_ABI sceFiosFHWriteSync(const void* op_attr, s32 handle, const void* buffer, s64 size) {
    static_cast<void>(op_attr);
    const s64 written = Kernel::sceKernelWrite(handle, buffer, static_cast<u64>(size));
    LOG_INFO(Lib_SysModule, "[FIOS-HLE][FHWriteSync] handle={} size={} written={}", handle, size,
             written);
    return written;
}

s32 PS4_SYSV_ABI sceFiosStatSync(const void* op_attr, const char* path, FiosStat* stat) {
    if (path == nullptr || stat == nullptr) {
        return -1;
    }
    s32 handle = -1;
    const s32 open_result = sceFiosFHOpenSync(op_attr, &handle, path, nullptr);
    if (open_result < 0) {
        return open_result;
    }
    const s32 result = sceFiosFHStatSync(op_attr, handle, stat);
    sceFiosFHCloseSync(op_attr, handle);
    return result;
}

// Async Fios2 op API. shadPS4 has no real async I/O queue, so async submitters do the
// work inline and hand back a fake op handle. The Op* waiters then report the op as
// already complete and return the recorded result. This mirrors what the desktop x86
// path achieves via AeroLib's benign no-op stubs, while keeping the byte counts intact.

s32 PS4_SYSV_ABI sceFiosOverlayAdd(void* overlay, s32* out_id) {
    LOG_INFO(Lib_SysModule, "[FIOS-HLE][OverlayAdd] overlay={} out_id={}", overlay,
             static_cast<void*>(out_id));
    // No overlay filesystem is implemented; accept and ignore the registration.
    if (out_id != nullptr) {
        *out_id = 1; // fake overlay id
    }
    return 0;
}

s32 PS4_SYSV_ABI sceFiosFHPread(const void* op_attr, s32 handle, void* buffer, s64 size,
                                s64 offset) {
    // Non-blocking positioned read: perform it synchronously and return a fake op handle
    // so a following OpWait/OpGetActualCount returns the real byte count. The op handle is
    // the return value (positive on success, negative errno on failure), matching the Vita/
    // PS4 SceFiosOp convention rather than a separate out-param.
    LogOpAttr("FHPread", op_attr);
    const s64 read = Kernel::sceKernelPread(handle, buffer, static_cast<u64>(size), offset);
    const s32 op = CompleteAsyncOp(op_attr, read);
    LOG_INFO(Lib_SysModule,
             "[FIOS-HLE][FHPread] handle={} offset={:#x} size={} read={} op={} op_attr={}", handle,
             offset, size, read, op, op_attr);
    return op;
}

s32 PS4_SYSV_ABI sceFiosFHRead(const void* op_attr, s32 handle, void* buffer, s64 size) {
    LogOpAttr("FHRead", op_attr);
    const s64 read = Kernel::sceKernelRead(handle, buffer, static_cast<u64>(size));
    const s32 op = CompleteAsyncOp(op_attr, read);
    LOG_INFO(Lib_SysModule,
             "[FIOS-HLE][FHRead] handle={} size={} read={} op={} op_attr={}", handle, size, read,
             op, op_attr);
    return op;
}

s32 PS4_SYSV_ABI sceFiosFHOpen(const void* op_attr, s32* out_handle, const char* path,
                               const void* open_params) {
    // Non-blocking open: run it synchronously (shadPS4 has no async queue), store the file
    // descriptor in *out_handle for the caller, and return a positive op handle so the
    // following OpWait/OpGetActualCount report success. Mirrors FHOpenSync but yields an op.
    // Also fires op_attr->pCallback if set (CUSA00223 waits on that, not OpWait).
    LogOpAttr("FHOpen", op_attr);
    if (out_handle == nullptr || path == nullptr) {
        return CompleteAsyncOp(op_attr, -1);
    }
    const s32 fd = Kernel::posix_open(path, 0, 0);
    if (fd < 0) {
        LOG_ERROR(Lib_SysModule, "[FIOS-HLE][FHOpen] failed path='{}'", path);
        return CompleteAsyncOp(op_attr, fd);
    }
    *out_handle = fd;
    const s32 op = CompleteAsyncOp(op_attr, 0);
    LOG_INFO(Lib_SysModule,
             "[FIOS-HLE][FHOpen] path='{}' handle={} op={} op_attr={} open_params={}", path, fd, op,
             op_attr, open_params);
    return op;
}

s32 PS4_SYSV_ABI sceFiosOpWait(s32 op) {
    LOG_INFO(Lib_SysModule, "[FIOS-HLE][OpWait] op={} (synchronous, already complete)", op);
    return 0;
}

s32 PS4_SYSV_ABI sceFiosOpWaitUntil(s32 op, s64 deadline) {
    // Deadline is absolute Fios time (us). Ops complete inline, so always done.
    LOG_INFO(Lib_SysModule,
             "[FIOS-HLE][OpWaitUntil] op={} deadline={} (synchronous, already complete)", op,
             deadline);
    return 0;
}

s32 PS4_SYSV_ABI sceFiosOpSyncWait(s32 op) {
    LOG_INFO(Lib_SysModule, "[FIOS-HLE][OpSyncWait] op={} (synchronous, already complete)", op);
    return 0;
}

s32 PS4_SYSV_ABI sceFiosGetDefaultOpAttr(void* out_attr) {
    if (out_attr == nullptr) {
        return -1;
    }
    std::memset(out_attr, 0, sizeof(FiosOpAttr));
    LOG_INFO(Lib_SysModule, "[FIOS-HLE][GetDefaultOpAttr] out={}", out_attr);
    return 0;
}

s64 PS4_SYSV_ABI sceFiosOpGetActualCount(s32 op) {
    const s64 count = QueryOpActualCount(op);
    LOG_INFO(Lib_SysModule, "[FIOS-HLE][OpGetActualCount] op={} count={}", op, count);
    return count;
}

s32 PS4_SYSV_ABI sceFiosOpIsDone(s32 op) {
    // All ops complete synchronously, so every submitted op is already done.
    LOG_INFO(Lib_SysModule, "[FIOS-HLE][OpIsDone] op={} (always done)", op);
    return 1;
}

s32 PS4_SYSV_ABI sceFiosOpGetError(s32 op) {
    // Ops store their outcome in the byte count: negative counts encode the errno.
    const s64 count = QueryOpActualCount(op);
    LOG_INFO(Lib_SysModule, "[FIOS-HLE][OpGetError] op={} error={}", op, count < 0 ? count : 0);
    return count < 0 ? static_cast<s32>(count) : 0;
}

s32 PS4_SYSV_ABI sceFiosOpDelete(s32 op) {
    // Drop the recorded count so fake op handles don't leak across long sessions.
    {
        std::scoped_lock lock{g_op_mutex};
        g_op_actual_counts.erase(op);
    }
    LOG_INFO(Lib_SysModule, "[FIOS-HLE][OpDelete] op={}", op);
    return 0;
}

s32 PS4_SYSV_ABI sceFiosFHClose(const void* op_attr, s32 handle) {
    // Async close: run synchronously and return a completed op handle.
    LogOpAttr("FHClose", op_attr);
    const s32 result = Kernel::posix_close(handle);
    const s32 op = CompleteAsyncOp(op_attr, result >= 0 ? 0 : result);
    LOG_INFO(Lib_SysModule, "[FIOS-HLE][FHClose] handle={} result={} op={} op_attr={}", handle,
             result, op, op_attr);
    return op;
}

// FIOS time unit is microseconds (Vita/PS4 libSceFios2 convention). Deadlines are
// typically `sceFiosTimeGetCurrent() + sceFiosTimeIntervalFromNanoseconds(timeout_ns)`.
// CUSA00223 (InFAMOUS Second Son) calls both at boot; FEX ENOSYS(38) left the guest
// with garbage timeouts and a black screen after early asset I/O (no GPU submit).
s64 PS4_SYSV_ABI sceFiosTimeGetCurrent() {
    using namespace std::chrono;
    const s64 us =
        duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
    LOG_TRACE(Lib_SysModule, "[FIOS-HLE][TimeGetCurrent] us={}", us);
    return us;
}

s64 PS4_SYSV_ABI sceFiosTimeIntervalFromNanoseconds(s64 nanoseconds) {
    const s64 us = nanoseconds / 1000;
    LOG_TRACE(Lib_SysModule, "[FIOS-HLE][TimeIntervalFromNanoseconds] ns={} us={}", nanoseconds,
              us);
    return us;
}

s64 PS4_SYSV_ABI sceFiosTimeIntervalToNanoseconds(s64 interval_us) {
    return interval_us * 1000;
}

s32 PS4_SYSV_ABI sceFiosTerminate() {
    LOG_INFO(Lib_SysModule, "[FIOS-HLE][Terminate]");
    return 0;
}

s32 PS4_SYSV_ABI sceFiosOpCancel(s32 op) {
    LOG_INFO(Lib_SysModule, "[FIOS-HLE][OpCancel] op={}", op);
    return 0;
}

s32 PS4_SYSV_ABI sceFiosIOFilterRemove(s32 index) {
    LOG_INFO(Lib_SysModule, "[FIOS-HLE][IOFilterRemove] index={}", index);
    return 0;
}

s32 PS4_SYSV_ABI sceFiosExistsSync(const void* op_attr, const char* path) {
    return sceFiosFileExistsSync(op_attr, path);
}

s32 PS4_SYSV_ABI sceFiosDeleteSync(const void* op_attr, const char* path) {
    static_cast<void>(op_attr);
    if (path == nullptr) {
        return -1;
    }
    const s32 result = Kernel::posix_unlink(path);
    LOG_INFO(Lib_SysModule, "[FIOS-HLE][DeleteSync] path='{}' result={}", path, result);
    // Missing file is fine for probe paths like delete_me.snt.
    return result < 0 ? 0 : result;
}

s32 PS4_SYSV_ABI sceFiosDelete(const void* op_attr, const char* path) {
    const s32 result = sceFiosDeleteSync(op_attr, path);
    return result >= 0 ? AllocateOpHandle(0) : result;
}

s32 PS4_SYSV_ABI sceFiosFileDeleteSync(const void* op_attr, const char* path) {
    return sceFiosDeleteSync(op_attr, path);
}

s32 PS4_SYSV_ABI sceFiosRenameSync(const void* op_attr, const char* old_path,
                                   const char* new_path) {
    static_cast<void>(op_attr);
    if (old_path == nullptr || new_path == nullptr) {
        return -1;
    }
    const s32 result = Kernel::posix_rename(old_path, new_path);
    LOG_INFO(Lib_SysModule, "[FIOS-HLE][RenameSync] '{}' -> '{}' result={}", old_path, new_path,
             result);
    return result;
}

s32 PS4_SYSV_ABI sceFiosFHSyncSync(const void* op_attr, s32 handle) {
    static_cast<void>(op_attr);
    static_cast<void>(handle);
    LOG_INFO(Lib_SysModule, "[FIOS-HLE][FHSyncSync] handle={} (no-op)", handle);
    return 0;
}

s32 PS4_SYSV_ABI sceFiosFHPwrite(const void* op_attr, s32 handle, const void* buffer, s64 size,
                                 s64 offset) {
    static_cast<void>(op_attr);
    // Prefer positioned write when available; fall back to seek+write.
    // sceKernelPwrite takes void* (non-const) for historical reasons.
    auto* writable = const_cast<void*>(buffer);
    s64 written = Kernel::sceKernelPwrite(handle, writable, static_cast<u64>(size), offset);
    if (written < 0) {
        const s64 pos = Kernel::posix_lseek(handle, offset, 0 /*SEEK_SET*/);
        if (pos < 0) {
            return static_cast<s32>(pos);
        }
        written = Kernel::sceKernelWrite(handle, buffer, static_cast<u64>(size));
    }
    const s32 op = written >= 0 ? AllocateOpHandle(written) : static_cast<s32>(written);
    LOG_INFO(Lib_SysModule,
             "[FIOS-HLE][FHPwrite] handle={} offset={:#x} size={} written={} op={}", handle, offset,
             size, written, op);
    return op;
}

s64 PS4_SYSV_ABI sceFiosArchiveGetMountBufferSize(const void* op_attr, const char* path,
                                                  const void* params) {
    return sceFiosArchiveGetMountBufferSizeSync(op_attr, path, params);
}

s32 PS4_SYSV_ABI sceFiosArchiveMount(const void* op_attr, s32* handle, const char* path,
                                     const char* mount_point, void* mount_buffer,
                                     s64 mount_buffer_size, s32 flags) {
    const s32 result = sceFiosArchiveMountSync(op_attr, handle, path, mount_point, mount_buffer,
                                               mount_buffer_size, flags);
    return result >= 0 ? AllocateOpHandle(0) : result;
}

s32 PS4_SYSV_ABI sceFiosArchiveUnmount(const void* op_attr, s32 handle) {
    static_cast<void>(op_attr);
    static_cast<void>(handle);
    LOG_INFO(Lib_SysModule, "[FIOS-HLE][ArchiveUnmount] handle={} (no-op)", handle);
    return 0;
}

s32 PS4_SYSV_ABI sceFiosArchiveUnmountSync(const void* op_attr, s32 handle) {
    return sceFiosArchiveUnmount(op_attr, handle);
}

s32 PS4_SYSV_ABI sceFiosDirectoryCreateSync(const void* op_attr, const char* path) {
    static_cast<void>(op_attr);
    LOG_INFO(Lib_SysModule, "[FIOS-HLE][DirectoryCreateSync] path='{}' (accept)",
             path ? path : "(null)");
    // Best-effort accept: host game dirs already exist for extracted dumps.
    return path != nullptr ? 0 : -1;
}

s32 PS4_SYSV_ABI sceFiosDirectoryCreate(const void* op_attr, const char* path) {
    const s32 result = sceFiosDirectoryCreateSync(op_attr, path);
    return result >= 0 ? AllocateOpHandle(0) : result;
}

// Directory-handle ops: not fully implemented. Return benign failures so callers
// fall back rather than ENOSYS-trap on FEX.
s32 PS4_SYSV_ABI sceFiosDHOpenSync(const void* op_attr, s32* handle, const char* path,
                                   void* open_params) {
    static_cast<void>(op_attr);
    static_cast<void>(open_params);
    LOG_WARNING(Lib_SysModule, "[FIOS-HLE][DHOpenSync] path='{}' stub", path ? path : "(null)");
    if (handle) {
        *handle = -1;
    }
    return -1;
}

s32 PS4_SYSV_ABI sceFiosDHOpen(const void* op_attr, s32* handle, const char* path,
                               void* open_params) {
    return sceFiosDHOpenSync(op_attr, handle, path, open_params);
}

s32 PS4_SYSV_ABI sceFiosDHReadSync(const void* op_attr, s32 handle, void* entry) {
    static_cast<void>(op_attr);
    static_cast<void>(handle);
    static_cast<void>(entry);
    return -1;
}

s32 PS4_SYSV_ABI sceFiosDHCloseSync(const void* op_attr, s32 handle) {
    static_cast<void>(op_attr);
    static_cast<void>(handle);
    return 0;
}

s32 PS4_SYSV_ABI sceFiosStat(const void* op_attr, const char* path, FiosStat* stat) {
    const s32 result = sceFiosStatSync(op_attr, path, stat);
    return result >= 0 ? AllocateOpHandle(0) : result;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("wAKZ-det+yo", "libSceFios2", 1, "libSceFios2", sceFiosInitialize);
    LIB_FUNCTION("b44anV2D7K0", "libSceFios2", 1, "libSceFios2", sceFiosFHOpenSync);
    LIB_FUNCTION("w13Ojm7ON9o", "libSceFios2", 1, "libSceFios2", sceFiosFHOpenWithModeSync);
    LIB_FUNCTION("Bn2ZF4ZjeuQ", "libSceFios2", 1, "libSceFios2", sceFiosFHReadSync);
    LIB_FUNCTION("2m9+Opco-hk", "libSceFios2", 1, "libSceFios2", sceFiosFHPreadSync);
    LIB_FUNCTION("xP45eIntEis", "libSceFios2", 1, "libSceFios2", sceFiosFHStatSync);
    LIB_FUNCTION("FdjoqFQOlt0", "libSceFios2", 1, "libSceFios2", sceFiosFHGetSize);
    LIB_FUNCTION("xReSebwKApA", "libSceFios2", 1, "libSceFios2", sceFiosFHSeek);
    LIB_FUNCTION("8IGjwtnvYwI", "libSceFios2", 1, "libSceFios2", sceFiosIsValidHandle);
    LIB_FUNCTION("AOujSGqU+ms", "libSceFios2", 1, "libSceFios2", sceFiosFHCloseSync);
    LIB_FUNCTION("lgITuBsRo2o", "libSceFios2", 1, "libSceFios2", sceFiosIOFilterAdd);
    LIB_FUNCTION("OIGbkgGOu6E", "libSceFios2", 1, "libSceFios2", sceFiosIOFilterPsarcDearchiver);
    LIB_FUNCTION("UUriaXy7G90", "libSceFios2", 1, "libSceFios2", sceFiosArchiveGetMountBufferSizeSync);
    LIB_FUNCTION("xutLbQdqyb4", "libSceFios2", 1, "libSceFios2", sceFiosArchiveMountSync);
    LIB_FUNCTION("zF8-CRvRXnM", "libSceFios2", 1, "libSceFios2", sceFiosFileGetSizeSync);
    LIB_FUNCTION("NwOHMRM2Ppw", "libSceFios2", 1, "libSceFios2", sceFiosFileExistsSync);
    LIB_FUNCTION("OOuvHKTu4Oc", "libSceFios2", 1, "libSceFios2", sceFiosDirectoryExistsSync);
    LIB_FUNCTION("MrRFrdgpsx8", "libSceFios2", 1, "libSceFios2", sceFiosFHTell);
    LIB_FUNCTION("Kl-TbrDU9YM", "libSceFios2", 1, "libSceFios2", sceFiosFHWriteSync);
    LIB_FUNCTION("jayvY07C5dk", "libSceFios2", 1, "libSceFios2", sceFiosStatSync);
    LIB_FUNCTION("TXABsmiiqto", "libSceFios2", 1, "libSceFios2", sceFiosOverlayAdd);
    LIB_FUNCTION("rR8wq7YFRZs", "libSceFios2", 1, "libSceFios2", sceFiosFHPread);
    LIB_FUNCTION("cg-VoPqZYss", "libSceFios2", 1, "libSceFios2", sceFiosFHRead);
    LIB_FUNCTION("er6TkQFUvp0", "libSceFios2", 1, "libSceFios2", sceFiosFHOpen);
    LIB_FUNCTION("SnoQQWnGK9I", "libSceFios2", 1, "libSceFios2", sceFiosOpWait);
    LIB_FUNCTION("ZSsFitZ4Kpk", "libSceFios2", 1, "libSceFios2", sceFiosOpWaitUntil);
    LIB_FUNCTION("+AGLl-l-WVE", "libSceFios2", 1, "libSceFios2", sceFiosGetDefaultOpAttr);
    LIB_FUNCTION("+FRvKknUj1I", "libSceFios2", 1, "libSceFios2", sceFiosOpGetActualCount);
    LIB_FUNCTION("2wvqS7Odb6M", "libSceFios2", 1, "libSceFios2", sceFiosOpSyncWait);
    LIB_FUNCTION("bfgo2Otmqz0", "libSceFios2", 1, "libSceFios2", sceFiosOpIsDone);
    LIB_FUNCTION("X+7rIfY97Ps", "libSceFios2", 1, "libSceFios2", sceFiosOpGetError);
    LIB_FUNCTION("5cyEcilO-J0", "libSceFios2", 1, "libSceFios2", sceFiosOpDelete);
    LIB_FUNCTION("5sYNBNK+W3g", "libSceFios2", 1, "libSceFios2", sceFiosFHClose);
    // CUSA00223 boot path: time + cleanup APIs (were STUB-only → FEX ENOSYS).
    LIB_FUNCTION("NUkBGOZARi4", "libSceFios2", 1, "libSceFios2", sceFiosTimeGetCurrent);
    LIB_FUNCTION("F1dCP7qkqok", "libSceFios2", 1, "libSceFios2", sceFiosTimeIntervalFromNanoseconds);
    LIB_FUNCTION("vZNIcB3n+bg", "libSceFios2", 1, "libSceFios2", sceFiosTimeIntervalToNanoseconds);
    LIB_FUNCTION("3HAgZPl1v+4", "libSceFios2", 1, "libSceFios2", sceFiosTerminate);
    LIB_FUNCTION("FA7dUleeGik", "libSceFios2", 1, "libSceFios2", sceFiosOpCancel);
    LIB_FUNCTION("ahIXyuwF0-o", "libSceFios2", 1, "libSceFios2", sceFiosIOFilterRemove);
    LIB_FUNCTION("2ZaHWy3IhKQ", "libSceFios2", 1, "libSceFios2", sceFiosExistsSync);
    LIB_FUNCTION("KsVTc04kN9k", "libSceFios2", 1, "libSceFios2", sceFiosDeleteSync);
    LIB_FUNCTION("nomcox0J32k", "libSceFios2", 1, "libSceFios2", sceFiosDelete);
    LIB_FUNCTION("bDupEgbQ6Fk", "libSceFios2", 1, "libSceFios2", sceFiosFileDeleteSync);
    LIB_FUNCTION("G-39lsdSgXo", "libSceFios2", 1, "libSceFios2", sceFiosRenameSync);
    LIB_FUNCTION("EzzSJz6yuMc", "libSceFios2", 1, "libSceFios2", sceFiosFHSyncSync);
    LIB_FUNCTION("PbxGVfOvUQY", "libSceFios2", 1, "libSceFios2", sceFiosFHPwrite);
    LIB_FUNCTION("ERmiOK9VT0g", "libSceFios2", 1, "libSceFios2", sceFiosArchiveGetMountBufferSize);
    LIB_FUNCTION("pIU8u6VsLM8", "libSceFios2", 1, "libSceFios2", sceFiosArchiveMount);
    LIB_FUNCTION("YfTBBU5nONQ", "libSceFios2", 1, "libSceFios2", sceFiosArchiveUnmount);
    LIB_FUNCTION("yy6C7k7FPZY", "libSceFios2", 1, "libSceFios2", sceFiosArchiveUnmountSync);
    LIB_FUNCTION("nWuza0ZdfqA", "libSceFios2", 1, "libSceFios2", sceFiosDirectoryCreateSync);
    LIB_FUNCTION("-ULUBK21QgE", "libSceFios2", 1, "libSceFios2", sceFiosDirectoryCreate);
    LIB_FUNCTION("GGqucL9F+YI", "libSceFios2", 1, "libSceFios2", sceFiosDHOpenSync);
    LIB_FUNCTION("uCkgJOrYUL4", "libSceFios2", 1, "libSceFios2", sceFiosDHOpen);
    LIB_FUNCTION("odjOGg8harg", "libSceFios2", 1, "libSceFios2", sceFiosDHReadSync);
    LIB_FUNCTION("0-p4O8FINmU", "libSceFios2", 1, "libSceFios2", sceFiosDHCloseSync);
    LIB_FUNCTION("QKsI9N7K1zE", "libSceFios2", 1, "libSceFios2", sceFiosStat);
}

} // namespace Libraries::Fios2
