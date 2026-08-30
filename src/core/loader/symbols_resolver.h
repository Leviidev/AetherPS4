// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>
#include "common/assert.h"
#include "common/types.h"

#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
namespace Core::GuestCpu {
class HleCallAdapter;
class HleCallRegistry;
} // namespace Core::GuestCpu
#endif

namespace Core::Loader {

enum class SymbolType {
    Unknown,
    Function,
    Object,
    Tls,
    NoType,
};

struct SymbolRecord {
    std::string name;
    std::string nid_name;
    u64 virtual_address;
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
    std::shared_ptr<GuestCpu::HleCallAdapter> hle_adapter;
    bool hle_fallback{};
#endif
};

struct SymbolResolver {
    std::string name;
    std::string nidName;
    std::string library;
    u16 library_version;
    std::string module;
    SymbolType type;
};

class SymbolsResolver {
public:
    SymbolsResolver();
    virtual ~SymbolsResolver();

    void AddSymbol(const SymbolResolver& s, u64 virtual_addr);
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
    void AddFunction(const SymbolResolver& s, std::shared_ptr<GuestCpu::HleCallAdapter> adapter);
    void AddFallbackFunction(const SymbolResolver& s,
                             std::shared_ptr<GuestCpu::HleCallAdapter> adapter);
    const std::shared_ptr<GuestCpu::HleCallAdapter>& AddUnsupportedFunction(const SymbolResolver& s);
    std::shared_ptr<GuestCpu::HleCallAdapter> FindFunction(u64 operation) const;
    GuestCpu::HleCallRegistry& GetHleCallRegistry();
#endif
    const SymbolRecord* FindSymbol(const SymbolResolver& s) const;

    void DebugDump(const std::filesystem::path& file_name);

    std::span<const SymbolRecord> GetSymbols() const {
        return m_symbols;
    }

    size_t GetSize() const noexcept {
        return m_symbols.size();
    }

    static std::string GenerateName(const SymbolResolver& s);

    static std::string_view SymbolTypeToS(SymbolType sym_type) {
        switch (sym_type) {
        case SymbolType::Unknown:
            return "Unknown";
        case SymbolType::Function:
            return "Function";
        case SymbolType::Object:
            return "Object";
        case SymbolType::Tls:
            return "Tls";
        case SymbolType::NoType:
            return "NoType";
        default:
            UNREACHABLE();
        }
    }

private:
    std::vector<SymbolRecord> m_symbols;
    // name (GenerateName's "nid#lib#version#module#type" key) -> index into m_symbols. Every
    // insertion into m_symbols must go through an index update alongside it (see AddSymbol/
    // AddFunction/AddUnsupportedFunction) -- this exists purely so FindSymbol (and the
    // internal existing-record checks in AddFunction/AddUnsupportedFunction) don't have to
    // linearly scan the whole vector, confirmed on-device as the real cause of a ~99 second
    // module-loading stall: thousands of relocations each doing an O(n) scan (with a fresh
    // fmt::format allocation per comparison) against a symbol table that itself grows into
    // the thousands during InitHLELibs. try_emplace (never overwrites an existing key) keeps
    // this pointing at the *first* entry for a given name, matching the original linear
    // scan's own first-match-wins behavior.
    std::unordered_map<std::string, size_t> m_symbol_index;
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
    std::unique_ptr<GuestCpu::HleCallRegistry> hle_registry;
#endif
};

} // namespace Core::Loader
