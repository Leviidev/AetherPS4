// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <fmt/format.h>
#include "common/io_file.h"
#include "common/string_util.h"
#include "common/types.h"
#include "core/aerolib/aerolib.h"
#include "core/loader/symbols_resolver.h"

#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
#include "core/guest_cpu/hle_call_adapter.h"
#endif

namespace Core::Loader {

SymbolsResolver::SymbolsResolver() = default;
SymbolsResolver::~SymbolsResolver() = default;

void SymbolsResolver::AddSymbol(const SymbolResolver& s, u64 virtual_addr) {
    m_symbols.emplace_back(GenerateName(s), s.nidName, virtual_addr);
}

#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
void SymbolsResolver::AddFunction(const SymbolResolver& s,
                                  std::shared_ptr<AetherPS4::GuestCpu::HleCallAdapter> adapter) {
    if (adapter == nullptr) {
        return;
    }
    const std::string name = GenerateName(s);
    adapter = GetHleCallRegistry().Register(std::move(adapter), name);
    if (adapter == nullptr) {
        return;
    }
    for (auto& record : m_symbols) {
        if (record.name == name && record.hle_fallback) {
            record.hle_adapter = std::move(adapter);
            record.hle_fallback = false;
            return;
        }
    }
    m_symbols.emplace_back(name, s.nidName, 0, std::move(adapter));
}

const std::shared_ptr<AetherPS4::GuestCpu::HleCallAdapter>&
SymbolsResolver::AddUnsupportedFunction(const SymbolResolver& s) {
    const std::string name = GenerateName(s);
    for (const auto& record : m_symbols) {
        if (record.name == name && record.hle_adapter != nullptr) {
            return record.hle_adapter;
        }
    }
    AddFunction(s, AetherPS4::GuestCpu::MakeUnsupportedHleCallAdapter());
    ASSERT_MSG(!m_symbols.empty() && m_symbols.back().name == name &&
                   m_symbols.back().hle_adapter != nullptr,
               "Unable to register unsupported HLE function {}", name);
    m_symbols.back().hle_fallback = true;
    return m_symbols.back().hle_adapter;
}

std::shared_ptr<AetherPS4::GuestCpu::HleCallAdapter> SymbolsResolver::FindFunction(u64 operation) const {
    if (hle_registry == nullptr) {
        return {};
    }
    return hle_registry->Find(operation);
}

AetherPS4::GuestCpu::HleCallRegistry& SymbolsResolver::GetHleCallRegistry() {
    if (hle_registry == nullptr) {
        hle_registry = std::make_unique<AetherPS4::GuestCpu::HleCallRegistry>();
    }
    return *hle_registry;
}
#endif

std::string SymbolsResolver::GenerateName(const SymbolResolver& s) {
    return fmt::format("{}#{}#{}#{}#{}", s.name, s.library, s.library_version, s.module,
                       SymbolTypeToS(s.type));
}

const SymbolRecord* SymbolsResolver::FindSymbol(const SymbolResolver& s) const {
    const std::string name = GenerateName(s);
    for (u32 i = 0; i < m_symbols.size(); i++) {
        if (m_symbols[i].name == name) {
            return &m_symbols[i];
        }
    }

    // LOG_INFO(Core_Linker, "Unresolved! {}", name);
    return nullptr;
}

void SymbolsResolver::DebugDump(const std::filesystem::path& file_name) {
    Common::FS::IOFile f{file_name, Common::FS::FileAccessMode::Create,
                         Common::FS::FileType::TextFile};
    for (const auto& symbol : m_symbols) {
        const auto ids = Common::SplitString(symbol.name, '#');
        const auto aeronid = AeroLib::FindByNid(ids.at(0).c_str());
        const auto nid_name = aeronid ? aeronid->name : "UNK";
        f.WriteString(fmt::format("0x{:<20x} {:<16} {:<60} {:<30} {:<2} {:<30} {:<10}\n",
                                  symbol.virtual_address, ids.at(0), nid_name, ids.at(1), ids.at(2),
                                  ids.at(3), ids.at(4)));
    }
}

} // namespace Core::Loader
