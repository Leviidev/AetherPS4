// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <boost/container/flat_map.hpp>
#include <xbyak/xbyak.h>
#include <xbyak/xbyak_util.h>
#include "common/arch.h"
#include "common/decoder.h"
#include "common/io_file.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/signal_context.h"
#include "core/emulator_settings.h"
#include "core/signals.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/breadth_first_search.h"
#include "shader_recompiler/ir/opcodes.h"
#include "shader_recompiler/ir/passes/srt.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/ir/reg.h"
#include "shader_recompiler/ir/srt_gvn_table.h"
#include "shader_recompiler/ir/value.h"

#if defined(ARCH_ARM64) && (defined(__linux__) || defined(__APPLE__))
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <unistd.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#include <pthread.h>
#if TARGET_OS_IPHONE
// iOS dual-mapped JIT for the ARM64 SRT walker JIT site.
// See src/core/ios/ios_jit_allocator.h for full BRK-trap protocol docs.
#include "core/ios/ios_jit_allocator.h"
// sys_icache_invalidate, not __builtin___clear_cache: this custom-generated Xcode
// project (generate_project.rb) doesn't auto-link compiler-rt's builtins library the
// way a normal Xcode-created project template does, so __builtin___clear_cache lowers
// to an unresolved `___clear_cache` symbol at link time. sys_icache_invalidate is
// Darwin's own documented API for the same operation and needs no separate runtime lib.
#include <libkern/OSCacheControl.h>
#endif
#endif
#endif

#ifdef ARCH_X86_64

using namespace Xbyak::util;

static std::unique_ptr<Xbyak::CodeGenerator> g_srt_codegen_ptr;
static const u8* g_srt_codegen_start = nullptr;

static Xbyak::CodeGenerator& GetSrtCodegen() {
    if (!g_srt_codegen_ptr) {
        g_srt_codegen_ptr = std::make_unique<Xbyak::CodeGenerator>(32_MB);
    }
    return *g_srt_codegen_ptr;
}

#define g_srt_codegen (GetSrtCodegen())

namespace Shader {

PFN_SrtWalker RegisterWalkerCode(const u8* ptr, size_t size) {
    const auto func_addr = (PFN_SrtWalker)g_srt_codegen.getCurr();
    g_srt_codegen.db(ptr, size);
    g_srt_codegen.ready();
    return func_addr;
}

} // namespace Shader

namespace {

static void DumpSrtProgram(const Shader::Info& info, const u8* code, size_t codesize) {
    using namespace Common::FS;

    const auto dump_dir = GetUserPath(PathType::ShaderDir) / "dumps";
    if (!std::filesystem::exists(dump_dir)) {
        std::filesystem::create_directories(dump_dir);
    }
    const auto filename = fmt::format("{}_{:#018x}.srtprogram.txt", info.stage, info.pgm_hash);
    const auto file = IOFile{dump_dir / filename, FileAccessMode::Create, FileType::TextFile};

    u64 address = reinterpret_cast<u64>(code);
    u64 code_end = address + codesize;
    ZydisDecodedInstruction instruction;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    ZyanStatus status = ZYAN_STATUS_SUCCESS;
    while (address < code_end && ZYAN_SUCCESS(Common::Decoder::Instance()->decodeInstruction(
                                     instruction, operands, reinterpret_cast<void*>(address)))) {
        std::string s =
            Common::Decoder::Instance()->disassembleInst(instruction, operands, address);
        s += "\n";
        file.WriteString(s);
        address += instruction.length;
    }
}

static bool SrtWalkerSignalHandler(void* context, void* fault_address) {
    // Only handle if the fault address is within the SRT code range
    const u8* code_start = g_srt_codegen_start;
    const u8* code_end = code_start + g_srt_codegen.getSize();
    const void* code = Common::GetRip(context);
    if (code < code_start || code >= code_end) {
        return false; // Not in SRT code range
    }

    // Patch instruction to zero register
    ZydisDecodedInstruction instruction;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    ZyanStatus status = Common::Decoder::Instance()->decodeInstruction(instruction, operands,
                                                                       const_cast<void*>(code), 15);

    ASSERT(ZYAN_SUCCESS(status) && instruction.mnemonic == ZYDIS_MNEMONIC_MOV &&
           operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
           operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY);

    size_t len = instruction.length;
    const size_t patch_size = 3;
    u8* code_patch = const_cast<u8*>(reinterpret_cast<const u8*>(code));

    // We can only encounter rdi or r10d as the first operand in a
    // fault memory access for SRT walker.
    switch (operands[0].reg.value) {
    case ZYDIS_REGISTER_RDI:
        // mov rdi, [rdi + (off_dw << 2)] -> xor rdi, rdi
        code_patch[0] = 0x48;
        code_patch[1] = 0x31;
        code_patch[2] = 0xFF;
        break;
    case ZYDIS_REGISTER_R10D:
        // mov r10d, [rdi + (off_dw << 2)] -> xor r10d, r10d
        code_patch[0] = 0x45;
        code_patch[1] = 0x31;
        code_patch[2] = 0xD2;
        break;
    default:
        UNREACHABLE_MSG("Unsupported register for SRT walker patch");
        return false;
    }

    // Fill nops
    memset(code_patch + patch_size, 0x90, len - patch_size);

    LOG_DEBUG(Render_Recompiler, "Patched SRT walker at {}", code);

    return true;
}

using namespace Shader;

struct PassInfo {
    // map offset to inst
    using PtrUserList = boost::container::flat_map<u32, Shader::IR::Inst*>;

    Optimization::SrtGvnTable gvn_table;
    // keys are GetUserData or ReadConst instructions that are used as pointers
    std::unordered_map<IR::Inst*, PtrUserList> pointer_uses;
    // GetUserData instructions corresponding to sgpr_base of SRT roots
    boost::container::small_flat_map<IR::ScalarReg, IR::Inst*, 1> srt_roots;

    // pick a single inst for a given value number
    std::unordered_map<u32, IR::Inst*> vn_to_inst;

    // Bumped during codegen to assign offsets to readconsts
    u32 dst_off_dw;

    PtrUserList* GetUsesAsPointer(IR::Inst* inst) {
        auto it = pointer_uses.find(inst);
        if (it != pointer_uses.end()) {
            return &it->second;
        }
        return nullptr;
    }

    // Return a single instruction that this instruction is identical to, according
    // to value number
    // The "original" is arbitrary. Here it's the first instruction found for a given value number
    IR::Inst* DeduplicateInstruction(IR::Inst* inst) {
        auto it = vn_to_inst.try_emplace(gvn_table.GetValueNumber(inst), inst);
        return it.first->second;
    }
};
} // namespace

namespace Shader::Optimization {

namespace {

static inline void PushPtr(Xbyak::CodeGenerator& c, u32 off_dw) {
    c.push(rdi);
    c.mov(rdi, ptr[rdi + (off_dw << 2)]);
    c.mov(r10, 0xFFFFFFFFFFFFULL);
    c.and_(rdi, r10);
}

static inline void PopPtr(Xbyak::CodeGenerator& c) {
    c.pop(rdi);
};

static void VisitPointer(u32 off_dw, IR::Inst* subtree, PassInfo& pass_info,
                         Xbyak::CodeGenerator& c) {
    PushPtr(c, off_dw);
    PassInfo::PtrUserList* use_list = pass_info.GetUsesAsPointer(subtree);
    ASSERT(use_list);

    // First copy all the src data from this tree level
    // That way, all data that was contiguous in the guest SRT is also contiguous in the
    // flattened buffer.
    // TODO src and dst are contiguous. Optimize with wider loads/stores
    // TODO if this subtree is dynamically indexed, don't compact it (keep it sparse)
    for (auto [src_off_dw, use] : *use_list) {
        c.mov(r10d, ptr[rdi + (src_off_dw << 2)]);
        c.mov(ptr[rsi + (pass_info.dst_off_dw << 2)], r10d);

        use->SetFlags<u32>(pass_info.dst_off_dw);
        pass_info.dst_off_dw++;
    }

    // Then visit any children used as pointers
    for (const auto [src_off_dw, use] : *use_list) {
        if (pass_info.GetUsesAsPointer(use)) {
            VisitPointer(src_off_dw, use, pass_info, c);
        }
    }

    PopPtr(c);
}

static void GenerateSrtProgram(Info& info, PassInfo& pass_info) {
    Xbyak::CodeGenerator& c = g_srt_codegen;

    if (pass_info.srt_roots.empty()) {
        return;
    }

    // Register the signal handler for SRT walker, if not already registered
    if (g_srt_codegen_start == nullptr) {
        g_srt_codegen_start = c.getCurr();
        auto* signals = Core::Signals::Instance();
        // Call after the memory invalidation handler
        constexpr u32 priority = 1;
        signals->RegisterAccessViolationHandler(SrtWalkerSignalHandler, priority);
    }

    info.srt_info.walker_func = c.getCurr<PFN_SrtWalker>();
    pass_info.dst_off_dw = NUM_USER_DATA_REGS;
    ASSERT(pass_info.dst_off_dw == info.srt_info.flattened_bufsize_dw);

    for (const auto& [sgpr_base, root] : pass_info.srt_roots) {
        VisitPointer(static_cast<u32>(sgpr_base), root, pass_info, c);
    }

    c.ret();
    c.ready();

    info.srt_info.walker_func_size =
        c.getCurr() - reinterpret_cast<const u8*>(info.srt_info.walker_func);

    if (EmulatorSettings.IsDumpShaders()) {
        DumpSrtProgram(info, reinterpret_cast<const u8*>(info.srt_info.walker_func),
                       info.srt_info.walker_func_size);
    }

    info.srt_info.flattened_bufsize_dw = pass_info.dst_off_dw;
}

}; // namespace

void FlattenExtendedUserdataPass(IR::Program& program) {
    Shader::Info& info = program.info;
    PassInfo pass_info;

    // traverse at end and assign offsets to duplicate readconsts, using
    // vn_to_inst as the source
    boost::container::small_vector<IR::Inst*, 32> all_readconsts;

    for (auto r_it = program.post_order_blocks.rbegin(); r_it != program.post_order_blocks.rend();
         r_it++) {
        IR::Block* block = *r_it;
        for (IR::Inst& inst : *block) {
            if (inst.GetOpcode() == IR::Opcode::ReadConst) {
                if (!inst.Arg(1).IsImmediate()) {
                    LOG_WARNING(Render_Recompiler, "ReadConst has non-immediate offset");
                    continue;
                }

                all_readconsts.push_back(&inst);
                if (pass_info.DeduplicateInstruction(&inst) != &inst) {
                    // This is a duplicate of a readconst we've already visited
                    continue;
                }

                IR::Inst* ptr_composite = inst.Arg(0).InstRecursive();

                const auto pred = [](IR::Inst* inst) -> std::optional<IR::Inst*> {
                    if (inst->GetOpcode() == IR::Opcode::GetUserData ||
                        inst->GetOpcode() == IR::Opcode::ReadConst) {
                        return inst;
                    }
                    return std::nullopt;
                };
                auto base0 = IR::BreadthFirstSearch(ptr_composite->Arg(0), pred);
                auto base1 = IR::BreadthFirstSearch(ptr_composite->Arg(1), pred);
                ASSERT_MSG(base0 && base1, "ReadConst not from constant memory");

                IR::Inst* ptr_lo = base0.value();
                ptr_lo = pass_info.DeduplicateInstruction(ptr_lo);

                auto ptr_uses_kv =
                    pass_info.pointer_uses.try_emplace(ptr_lo, PassInfo::PtrUserList{});
                PassInfo::PtrUserList& user_list = ptr_uses_kv.first->second;

                user_list[inst.Arg(1).U32()] = &inst;

                if (ptr_lo->GetOpcode() == IR::Opcode::GetUserData) {
                    IR::ScalarReg ud_reg = ptr_lo->Arg(0).ScalarReg();
                    pass_info.srt_roots[ud_reg] = ptr_lo;
                }
            }
        }
    }

    GenerateSrtProgram(info, pass_info);

    // Assign offsets to duplicate readconsts
    for (IR::Inst* readconst : all_readconsts) {
        ASSERT(pass_info.vn_to_inst.contains(pass_info.gvn_table.GetValueNumber(readconst)));
        IR::Inst* original = pass_info.DeduplicateInstruction(readconst);
        readconst->SetFlags<u32>(original->Flags<u32>());
    }

    info.RefreshFlatBuf();
}

} // namespace Shader::Optimization

#elif defined(ARCH_ARM64) && (defined(__linux__) || defined(__APPLE__))

namespace {

constexpr u32 Arm64MovX2X0 = 0xaa0003e2;
constexpr u32 Arm64PushX2 = 0xf81f0fe2;
constexpr u32 Arm64PopX2 = 0xf84107e2;
constexpr u32 Arm64LoadPointer = 0xf8646842;
constexpr u32 Arm64MaskPointer = 0xd340bc42;
constexpr u32 Arm64LoadDataRegisterOffset = 0xb8646843;
constexpr u32 Arm64StoreDataRegisterOffset = 0xb8256823;
constexpr u32 Arm64Ret = 0xd65f03c0;
constexpr size_t MaxSrtCodeRanges = 32768;

struct SrtCodeRange {
    uintptr_t begin{};
    uintptr_t end{};
};

struct SrtCodeMapping {
    u8* data{};     ///< RX (exec) address — used as the function pointer. May differ from
                    ///< rw_data on iOS where RW and RX are distinct virtual addresses.
    size_t size{};
#if defined(__APPLE__) && TARGET_OS_IPHONE
    u8* rw_data{};  ///< RW (write) address — only set on iOS; munmap'd on destruction.
                    ///< Null on non-iOS platforms (data == mmap addr there).
#endif

    ~SrtCodeMapping() {
#if defined(__APPLE__) && TARGET_OS_IPHONE
        // On iOS the mmap-owned RW address is rw_data (data holds the RX addr which
        // is managed by the kernel's vm_map entry and must NOT be munmap'd here).
        if (rw_data != nullptr) {
            munmap(rw_data, size);
        }
#else
        if (data != nullptr) {
            munmap(data, size);
        }
#endif
    }
};

std::array<SrtCodeRange, MaxSrtCodeRanges> g_srt_code_ranges{};
std::atomic_size_t g_srt_code_range_count{};
std::mutex g_srt_code_mutex;
std::vector<std::unique_ptr<SrtCodeMapping>> g_srt_code_mappings;
std::once_flag g_srt_signal_once;

bool IsSrtCodeAddress(uintptr_t pc) {
    const size_t count = g_srt_code_range_count.load(std::memory_order_acquire);
    for (size_t index = 0; index < count; ++index) {
        const auto& range = g_srt_code_ranges[index];
        if (pc >= range.begin && pc < range.end) {
            return true;
        }
    }
    return false;
}

// Both Linux and Darwin ARM64 ucontext_t expose a flat 0-30 GPR view here --
// Linux via uc_mcontext.regs[], Darwin via uc_mcontext->__ss.__x[] (only x29/
// x30 need special-casing on Darwin, and this walker never touches those).
void SetGprZero(void* context, int reg_index) {
#ifdef __APPLE__
    static_cast<ucontext_t*>(context)->uc_mcontext->__ss.__x[reg_index] = 0;
#else
    static_cast<ucontext_t*>(context)->uc_mcontext.regs[reg_index] = 0;
#endif
}

bool SrtWalkerSignalHandler(void* context, void* fault_address) {
    const auto pc = reinterpret_cast<uintptr_t>(Common::GetRip(context));
    if (!IsSrtCodeAddress(pc)) {
        return false;
    }

    u32 instruction{};
    std::memcpy(&instruction, reinterpret_cast<const void*>(pc), sizeof(instruction));
    if (instruction == Arm64LoadPointer) {
        SetGprZero(context, 2);
    } else if ((instruction & 0xffc003ffu) == 0xb9400043u ||
               instruction == Arm64LoadDataRegisterOffset) {
        SetGprZero(context, 3);
    } else {
        return false;
    }
    Common::IncrementRip(context, sizeof(u32));
    return true;
}

bool IsArm64SrtWalker(const u8* ptr, size_t size) {
    if (ptr == nullptr || size < sizeof(u32) * 2 || size % sizeof(u32) != 0) {
        return false;
    }
    u32 first{};
    u32 last{};
    std::memcpy(&first, ptr, sizeof(first));
    std::memcpy(&last, ptr + size - sizeof(last), sizeof(last));
    return first == Arm64MovX2X0 && last == Arm64Ret;
}

class Arm64SrtEmitter {
public:
    void Begin() {
        Emit(Arm64MovX2X0);
    }

    void PushPointer(u32 offset_dw) {
        Emit(Arm64PushX2);
        MoveImmediate(4, static_cast<u64>(offset_dw) * sizeof(u32));
        Emit(Arm64LoadPointer);
        Emit(Arm64MaskPointer);
    }

    void PopPointer() {
        Emit(Arm64PopX2);
    }

    void CopyDword(u32 source_offset_dw, u32 destination_offset_dw) {
        if (source_offset_dw <= 0xfff) {
            Emit(0xb9400043u | (source_offset_dw << 10));
        } else {
            MoveImmediate(4, static_cast<u64>(source_offset_dw) * sizeof(u32));
            Emit(Arm64LoadDataRegisterOffset);
        }

        if (destination_offset_dw <= 0xfff) {
            Emit(0xb9000023u | (destination_offset_dw << 10));
        } else {
            MoveImmediate(5, static_cast<u64>(destination_offset_dw) * sizeof(u32));
            Emit(Arm64StoreDataRegisterOffset);
        }
    }

    void End() {
        Emit(Arm64Ret);
    }

    const u8* Data() const {
        return reinterpret_cast<const u8*>(code.data());
    }

    size_t Size() const {
        return code.size() * sizeof(u32);
    }

private:
    void Emit(u32 instruction) {
        code.push_back(instruction);
    }

    void MoveImmediate(u32 reg, u64 value) {
        Emit(0xd2800000u | reg | (static_cast<u32>(value & 0xffff) << 5));
        for (u32 halfword = 1; halfword < 4; ++halfword) {
            const u32 part = static_cast<u32>((value >> (halfword * 16)) & 0xffff);
            if (part != 0) {
                Emit(0xf2800000u | reg | (halfword << 21) | (part << 5));
            }
        }
    }

    std::vector<u32> code;
};

using namespace Shader;

struct PassInfo {
    using PtrUserList = boost::container::flat_map<u32, Shader::IR::Inst*>;

    Optimization::SrtGvnTable gvn_table;
    std::unordered_map<IR::Inst*, PtrUserList> pointer_uses;
    boost::container::small_flat_map<IR::ScalarReg, IR::Inst*, 1> srt_roots;
    std::unordered_map<u32, IR::Inst*> vn_to_inst;
    u32 dst_off_dw;

    PtrUserList* GetUsesAsPointer(IR::Inst* inst) {
        auto it = pointer_uses.find(inst);
        return it != pointer_uses.end() ? &it->second : nullptr;
    }

    IR::Inst* DeduplicateInstruction(IR::Inst* inst) {
        auto it = vn_to_inst.try_emplace(gvn_table.GetValueNumber(inst), inst);
        return it.first->second;
    }
};

} // namespace

namespace Shader {

PFN_SrtWalker RegisterWalkerCode(const u8* ptr, size_t size) {
    if (!IsArm64SrtWalker(ptr, size)) {
        LOG_WARNING(Render_Recompiler,
                    "Ignoring incompatible cached SRT walker; shader will be recompiled");
        return nullptr;
    }

    std::lock_guard lock{g_srt_code_mutex};
    const size_t range_index = g_srt_code_range_count.load(std::memory_order_relaxed);
    if (range_index >= g_srt_code_ranges.size()) {
        LOG_CRITICAL(Render_Recompiler, "ARM64 SRT walker range table is full");
        std::abort();
    }

    const long page_size_result = sysconf(_SC_PAGESIZE);
    if (page_size_result <= 0) {
        LOG_CRITICAL(Render_Recompiler, "Unable to query host page size for ARM64 SRT walker");
        std::abort();
    }
    const size_t page_size = static_cast<size_t>(page_size_result);
    const size_t mapping_size = (size + page_size - 1) & ~(page_size - 1);
    auto mapping = std::make_unique<SrtCodeMapping>();
#if defined(__APPLE__) && TARGET_OS_IPHONE
    // iOS dual-mapped JIT for the ARM64 SRT walker.
    //
    // iOS rejects mprotect(PROT_EXEC) from an unentitled app unconditionally.
    // Instead we use BreakpointJIT.framework's BRK-trap protocol (brk #0xf00d,
    // x16=1) to ask the attached StikDebug debugger to dual-map these physical
    // pages: one writable (RW) virtual address for memcpy, one executable (RX)
    // virtual address for the CPU to branch to. See ios_jit_allocator.h.
    //
    // We pass nullptr as the `addr` argument so BreakGetJITMapping allocates both
    // the RW and RX mappings itself and returns the RX address. This avoids a
    // separate mmap() call and matches the "allocate new" protocol branch
    // (JIT26PrepareRegion: x0==0 → `_M{size},rx` → allocate fresh).
    //
    // Important: BreakJITDetach() must NOT be called here because this function
    // may be called multiple times (once per shader that uses an SRT walker).
    // The Swift host (Phase 4, JITSupport.swift) calls it once after shadps4_init,
    // or fex_guest_engine.cpp's GuestEngine::Create calls it after its own setup.
    // Calling it mid-shader-compilation would detach the debugger early, breaking
    // subsequent BreakGetJITMapping calls.
    {
        // region.rw_addr and region.rx_addr are genuinely different virtual addresses backed
        // by the same physical pages -- see ios_jit_allocator.h's top-of-file comment.
        Core::DualMappedRegion region = Core::DualMappedRegion::Allocate(mapping_size);
        if (!region.IsValid()) {
            LOG_CRITICAL(Render_Recompiler,
                         "ARM64 SRT walker: BreakGetJITMapping({} bytes) returned nullptr. "
                         "StikDebug with the Universal JIT Script (2026-03-29+) must be "
                         "attached before shaders start compiling. "
                         "See Phase 4 of the iOS port plan.", mapping_size);
            std::abort();
        }
        // Write the walker machine code through the RW address.
        std::memcpy(region.rw_addr, ptr, size);
        // Flush CPU instruction cache on the RW range so the icache sees the new code
        // when it fetches through the aliased RX address.
        sys_icache_invalidate(region.rw_addr, size);
        // Store the RX address in mapping->data (the exec function pointer) and the
        // RW address in mapping->rw_data (for munmap in ~SrtCodeMapping).
        mapping->data    = region.rx_addr;
        mapping->rw_data = region.rw_addr;
        mapping->size    = mapping_size;
        // Transfer ownership of the RW address away from `region` so ~DualMappedRegion
        // does NOT munmap it (SrtCodeMapping's destructor will do it via rw_data).
        region.rw_addr = nullptr;
    }
#elif defined(__APPLE__)
    // Apple Silicon rejects PROT_EXEC on a plain mmap/mprotect outright
    // (EACCES) -- an executable allocation must carry MAP_JIT, and writing to
    // it afterward requires this thread to toggle pthread_jit_write_protect_np(0)
    // first (per-thread W^X state, not per-mapping; see the identical pattern
    // in FEXCore's ScopedJITWriteProtect / AllocatorHooks.h). All three prot
    // bits are requested upfront since MAP_JIT pages don't need a separate
    // mprotect step -- only the per-thread toggle governs actual access.
    mapping->data = static_cast<u8*>(mmap(nullptr, mapping_size, PROT_READ | PROT_WRITE | PROT_EXEC,
                                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0));
    mapping->size = mapping_size;
    if (mapping->data == MAP_FAILED) {
        mapping->data = nullptr;
        LOG_CRITICAL(Render_Recompiler, "Unable to allocate ARM64 SRT walker: errno {}", errno);
        std::abort();
    }
    pthread_jit_write_protect_np(0);
    std::memcpy(mapping->data, ptr, size);
    pthread_jit_write_protect_np(1);
    __builtin___clear_cache(reinterpret_cast<char*>(mapping->data),
                            reinterpret_cast<char*>(mapping->data + size));
#else
    mapping->data = static_cast<u8*>(
        mmap(nullptr, mapping_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    mapping->size = mapping_size;
    if (mapping->data == MAP_FAILED) {
        mapping->data = nullptr;
        LOG_CRITICAL(Render_Recompiler, "Unable to allocate ARM64 SRT walker: errno {}", errno);
        std::abort();
    }
    std::memcpy(mapping->data, ptr, size);
    __builtin___clear_cache(reinterpret_cast<char*>(mapping->data),
                            reinterpret_cast<char*>(mapping->data + size));
    if (mprotect(mapping->data, mapping_size, PROT_READ | PROT_EXEC) != 0) {
        LOG_CRITICAL(Render_Recompiler, "Unable to protect ARM64 SRT walker: errno {}", errno);
        std::abort();
    }
#endif

    std::call_once(g_srt_signal_once, [] {
        constexpr u32 priority = 1;
        Core::Signals::Instance()->RegisterAccessViolationHandler(SrtWalkerSignalHandler, priority);
    });

    const auto begin = reinterpret_cast<uintptr_t>(mapping->data);
    auto* function = reinterpret_cast<PFN_SrtWalker>(mapping->data);
    g_srt_code_mappings.push_back(std::move(mapping));
    g_srt_code_ranges[range_index] = {begin, begin + size};
    g_srt_code_range_count.store(range_index + 1, std::memory_order_release);
    return function;
}

} // namespace Shader

namespace Shader::Optimization {

namespace {

void VisitPointer(u32 offset_dw, IR::Inst* subtree, PassInfo& pass_info,
                  Arm64SrtEmitter& emitter) {
    emitter.PushPointer(offset_dw);
    PassInfo::PtrUserList* use_list = pass_info.GetUsesAsPointer(subtree);
    ASSERT(use_list);

    for (auto [source_offset_dw, use] : *use_list) {
        emitter.CopyDword(source_offset_dw, pass_info.dst_off_dw);
        use->SetFlags<u32>(pass_info.dst_off_dw++);
    }

    for (const auto [source_offset_dw, use] : *use_list) {
        if (pass_info.GetUsesAsPointer(use)) {
            VisitPointer(source_offset_dw, use, pass_info, emitter);
        }
    }
    emitter.PopPointer();
}

void GenerateSrtProgram(Info& info, PassInfo& pass_info) {
    if (pass_info.srt_roots.empty()) {
        return;
    }

    Arm64SrtEmitter emitter;
    emitter.Begin();
    pass_info.dst_off_dw = NUM_USER_DATA_REGS;
    ASSERT(pass_info.dst_off_dw == info.srt_info.flattened_bufsize_dw);
    for (const auto& [sgpr_base, root] : pass_info.srt_roots) {
        VisitPointer(static_cast<u32>(sgpr_base), root, pass_info, emitter);
    }
    emitter.End();

    info.srt_info.walker_func = RegisterWalkerCode(emitter.Data(), emitter.Size());
    ASSERT(info.srt_info.walker_func != nullptr);
    info.srt_info.walker_func_size = emitter.Size();
    info.srt_info.flattened_bufsize_dw = pass_info.dst_off_dw;
}

} // namespace

void FlattenExtendedUserdataPass(IR::Program& program) {
    Shader::Info& info = program.info;
    PassInfo pass_info;
    boost::container::small_vector<IR::Inst*, 32> all_readconsts;

    for (auto r_it = program.post_order_blocks.rbegin(); r_it != program.post_order_blocks.rend();
         ++r_it) {
        IR::Block* block = *r_it;
        for (IR::Inst& inst : *block) {
            if (inst.GetOpcode() != IR::Opcode::ReadConst) {
                continue;
            }
            if (!inst.Arg(1).IsImmediate()) {
                LOG_WARNING(Render_Recompiler, "ReadConst has non-immediate offset");
                continue;
            }

            all_readconsts.push_back(&inst);
            if (pass_info.DeduplicateInstruction(&inst) != &inst) {
                continue;
            }

            IR::Inst* ptr_composite = inst.Arg(0).InstRecursive();
            const auto pred = [](IR::Inst* candidate) -> std::optional<IR::Inst*> {
                if (candidate->GetOpcode() == IR::Opcode::GetUserData ||
                    candidate->GetOpcode() == IR::Opcode::ReadConst) {
                    return candidate;
                }
                return std::nullopt;
            };
            auto base0 = IR::BreadthFirstSearch(ptr_composite->Arg(0), pred);
            auto base1 = IR::BreadthFirstSearch(ptr_composite->Arg(1), pred);
            ASSERT_MSG(base0 && base1, "ReadConst not from constant memory");

            IR::Inst* ptr_lo = pass_info.DeduplicateInstruction(base0.value());
            auto [uses, inserted] =
                pass_info.pointer_uses.try_emplace(ptr_lo, PassInfo::PtrUserList{});
            uses->second[inst.Arg(1).U32()] = &inst;
            if (ptr_lo->GetOpcode() == IR::Opcode::GetUserData) {
                pass_info.srt_roots[ptr_lo->Arg(0).ScalarReg()] = ptr_lo;
            }
        }
    }

    GenerateSrtProgram(info, pass_info);
    for (IR::Inst* readconst : all_readconsts) {
        ASSERT(pass_info.vn_to_inst.contains(pass_info.gvn_table.GetValueNumber(readconst)));
        IR::Inst* original = pass_info.DeduplicateInstruction(readconst);
        readconst->SetFlags<u32>(original->Flags<u32>());
    }
    info.RefreshFlatBuf();
}

} // namespace Shader::Optimization

#else

namespace Shader {

PFN_SrtWalker RegisterWalkerCode(const u8* ptr, size_t size) {
    UNREACHABLE_MSG("RegisterWalkerCode unimplemented for target architecture.");
}

namespace Optimization {

void FlattenExtendedUserdataPass(IR::Program& program) {
    UNREACHABLE_MSG("FlattenExtendedUserdataPass unimplemented for target architecture.");
}

} // namespace Optimization

} // namespace Shader

#endif
