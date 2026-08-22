// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/loader/elf.h"
#include "core/loader/symbols_resolver.h"
#include "core/tls.h"
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
#include "core/guest_cpu/hle_call_adapter.h"
#endif

#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
#define LIB_FUNCTION(nid, lib, libversion, mod, function)                                          \
    {                                                                                              \
        Core::Loader::SymbolResolver sr{};                                                         \
        sr.name = nid;                                                                             \
        sr.library = lib;                                                                          \
        sr.library_version = libversion;                                                           \
        sr.module = mod;                                                                           \
        sr.type = Core::Loader::SymbolType::Function;                                              \
        /* FEX reaches HLE through an x86 syscall veneer, never an ARM host address. */           \
        sym->AddFunction(sr, AetherPS4::GuestCpu::MakeHleCallAdapter(function));                        \
    }
#else
#define LIB_FUNCTION(nid, lib, libversion, mod, function)                                          \
    {                                                                                              \
        Core::Loader::SymbolResolver sr{};                                                         \
        sr.name = nid;                                                                             \
        sr.library = lib;                                                                          \
        sr.library_version = libversion;                                                           \
        sr.module = mod;                                                                           \
        sr.type = Core::Loader::SymbolType::Function;                                              \
        auto func = reinterpret_cast<u64>(HOST_CALL(function));                                    \
        sym->AddSymbol(sr, func);                                                                  \
    }
#endif

#define LIB_OBJ(nid, lib, libversion, mod, obj)                                                    \
    {                                                                                              \
        Core::Loader::SymbolResolver sr{};                                                         \
        sr.name = nid;                                                                             \
        sr.library = lib;                                                                          \
        sr.library_version = libversion;                                                           \
        sr.module = mod;                                                                           \
        sr.type = Core::Loader::SymbolType::Object;                                                \
        sym->AddSymbol(sr, reinterpret_cast<u64>(obj));                                            \
    }

namespace Libraries {

void InitHLELibs(Core::Loader::SymbolsResolver* sym);

} // namespace Libraries
