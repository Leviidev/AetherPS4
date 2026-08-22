// SPDX-License-Identifier: MIT
#pragma once

#include <FEXCore/Utils/LogManager.h>

#include <cstdint>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#if !defined(_WIN32) && !defined(__APPLE__)
#include <syscall.h>
#elif defined(_WIN32)
#include <processthreadsapi.h>
#endif
#ifdef __APPLE__
#include <pthread.h>
#endif
#include <sys/stat.h>
#include <unistd.h>

namespace FHU::Syscalls {
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#ifndef SEM_STAT_ANY
#define SEM_STAT_ANY 20
#endif

#ifndef SHM_STAT_ANY
#define SHM_STAT_ANY 15
#endif

#ifndef MSG_STAT_ANY
#define MSG_STAT_ANY 13
#endif

#ifndef CLONE_PIDFD
#define CLONE_PIDFD 0x00001000
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
#ifndef SYS_statx
#define SYS_statx 291
#endif
#elif defined(__x86_64__) || defined(_M_X64)
#ifndef SYS_statx
#define SYS_statx 332
#endif
#endif

// Common syscall numbers
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif

#ifdef __APPLE__
// Only getcpu() and gettid() are actually used anywhere within the FEXCore-only (non-Linux-
// syscall-emulation-layer) build: getcpu() for a plausible-looking CPUID leaf during guest
// CPUID emulation (doesn't need to be live-accurate, same as this file's own Windows fallback
// below which always reports node 0), and gettid() purely for a debug JIT-symbol-naming
// string. tgkill/statx/renameat2/pidfd_open have no callers in this build configuration --
// stubbed the same "unsupported" way this file's own Windows branch already stubs tgkill.
inline int32_t getcpu(uint32_t* cpu, uint32_t* node) {
  // Darwin doesn't expose which logical CPU a thread is currently running on via any stable
  // public API (by design, for scheduler opacity) -- 0 is a safe, always-valid placeholder,
  // matching how this file's own Windows branch always reports node 0 below.
  if (cpu) {
    *cpu = 0;
  }
  if (node) {
    *node = 0;
  }
  return 0;
}

inline int32_t gettid() {
  uint64_t Tid {};
  pthread_threadid_np(nullptr, &Tid);
  return static_cast<int32_t>(Tid);
}

inline int32_t tgkill(pid_t tgid, pid_t tid, int sig) {
  ERROR_AND_DIE_FMT("Unsupported");
  return 0;
}

inline int32_t statx(int dirfd, const char* pathname, int32_t flags, uint32_t mask, void* statxbuf) {
  ERROR_AND_DIE_FMT("Unsupported");
  return -1;
}

inline int32_t renameat2(int olddirfd, const char* oldpath, int newdirfd, const char* newpath, unsigned int flags) {
  ERROR_AND_DIE_FMT("Unsupported");
  return -1;
}

inline int32_t pidfd_open(pid_t pid, unsigned int flags) {
  ERROR_AND_DIE_FMT("Unsupported");
  return -1;
}
#elif !defined(_WIN32)
inline int32_t getcpu(uint32_t* cpu, uint32_t* node) {
  // Third argument is unused
#if defined(HAS_SYSCALL_GETCPU) && HAS_SYSCALL_GETCPU
  return ::getcpu(cpu, node);
#else
  return ::syscall(SYS_getcpu, cpu, node, nullptr);
#endif
}

inline int32_t gettid() {
#if defined(HAS_SYSCALL_GETTID) && HAS_SYSCALL_GETTID
  return ::gettid();
#else
  return ::syscall(SYS_gettid);
#endif
}

inline int32_t tgkill(pid_t tgid, pid_t tid, int sig) {
#if defined(HAS_SYSCALL_TGKILL) && HAS_SYSCALL_TGKILL
  return ::tgkill(tgid, tid, sig);
#else
  return ::syscall(SYS_tgkill, tgid, tid, sig);
#endif
}

inline int32_t statx(int dirfd, const char* pathname, int32_t flags, uint32_t mask, void* statxbuf) {
#if defined(HAS_SYSCALL_STATX) && HAS_SYSCALL_STATX
  return ::statx(dirfd, pathname, flags, mask, reinterpret_cast<struct statx* __restrict>(statxbuf));
#else
  return ::syscall(SYS_statx, dirfd, pathname, flags, mask, statxbuf);
#endif
}

inline int32_t renameat2(int olddirfd, const char* oldpath, int newdirfd, const char* newpath, unsigned int flags) {
#if defined(HAS_SYSCALL_RENAMEAT2) && HAS_SYSCALL_RENAMEAT2
  return ::renameat2(olddirfd, oldpath, newdirfd, newpath, flags);
#else
  return ::syscall(SYS_renameat2, olddirfd, oldpath, newdirfd, newpath, flags);
#endif
}

inline int32_t pidfd_open(pid_t pid, unsigned int flags) {
  return ::syscall(SYS_pidfd_open, pid, flags);
}
#else

inline int32_t getcpu(uint32_t* cpu, uint32_t* node) {
  if (cpu) {
    *cpu = GetCurrentProcessorNumber();
  }
  if (node) {
    *node = 0;
  }
  return 0;
}

inline int32_t tgkill(pid_t tgid, pid_t tid, int sig) {
  ERROR_AND_DIE_FMT("Unsupported");
  return 0;
}

inline int32_t gettid() {
  return GetCurrentThreadId();
}

#endif

} // namespace FHU::Syscalls
