// PKG stream extract for Android (adapted from shadPS4 PKG::Extract / ExtractFiles).
#include "pkg_extractor.h"
#include "pkg_crypto.h"
#include "pkg_type.h"
#include "pfs.h"
#include "types.h"

#include <android/log.h>
#include <unistd.h>
#include <zlib.h>
#if defined(BACHATA_HAVE_LIBDEFLATE)
#include <libdeflate.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "BachataPkg", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "BachataPkg", __VA_ARGS__)

namespace fs = std::filesystem;
namespace bachata_pkg {
namespace {

std::atomic<bool> g_cancel{false};

struct PKGHeader {
    u32_be magic;
    u32_be pkg_type;
    u32_be pkg_0x8;
    u32_be pkg_file_count;
    u32_be pkg_table_entry_count;
    u16_be pkg_sc_entry_count;
    u16_be pkg_table_entry_count_2;
    u32_be pkg_table_entry_offset;
    u32_be pkg_sc_entry_data_size;
    u64_be pkg_body_offset;
    u64_be pkg_body_size;
    u64_be pkg_content_offset;
    u64_be pkg_content_size;
    u8 pkg_content_id[0x24];
    u8 pkg_padding[0xC];
    u32_be pkg_drm_type;
    u32_be pkg_content_type;
    u32_be pkg_content_flags;
    u32_be pkg_promote_size;
    u32_be pkg_version_date;
    u32_be pkg_version_hash;
    u32_be pkg_0x088;
    u32_be pkg_0x08C;
    u32_be pkg_0x090;
    u32_be pkg_0x094;
    u32_be pkg_iro_tag;
    u32_be pkg_drm_type_version;
    u8 pkg_zeroes_1[0x60];
    u8 digest_entries1[0x20];
    u8 digest_entries2[0x20];
    u8 digest_table_digest[0x20];
    u8 digest_body_digest[0x20];
    u8 pkg_zeroes_2[0x280];
    u32_be pkg_0x400;
    u32_be pfs_image_count;
    u64_be pfs_image_flags;
    u64_be pfs_image_offset;
    u64_be pfs_image_size;
    u64_be mount_image_offset;
    u64_be mount_image_size;
    u64_be pkg_size;
    u32_be pfs_signed_size;
    u32_be pfs_cache_size;
    u8 pfs_image_digest[0x20];
    u8 pfs_signed_digest[0x20];
    u64_be pfs_split_size_nth_0;
    u64_be pfs_split_size_nth_1;
};

struct PKGEntry {
    u32_be id;
    u32_be filename_offset;
    u32_be flags1;
    u32_be flags2;
    u32_be offset;
    u32_be size;
    u64_be padding;
};
static_assert(sizeof(PKGEntry) == 32);

bool pread_all(int fd, void* buf, size_t n, off_t off) {
    auto* p = static_cast<uint8_t*>(buf);
    size_t got = 0;
    while (got < n) {
        const ssize_t r = pread(fd, p + got, n - got, off + static_cast<off_t>(got));
        if (r <= 0) return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

// Decompress one PFSC block (zlib format) into `decompressed`. Returns false on
// any error. The legacy zlib path discarded inflate()'s return value; this now
// surfaces corruption as a hard extract failure.
bool DecompressPFSC(std::span<char> compressed, std::span<char> decompressed) {
#if defined(BACHATA_HAVE_LIBDEFLATE)
    // libdeflate: ~2x faster single-shot inflate than stock zlib. A fresh
    // decompressor per call is cheap (no per-block allocation); the hot path
    // is the actual inflate, not the alloc.
    struct libdeflate_decompressor* d = libdeflate_alloc_decompressor();
    if (d == nullptr) return false;
    size_t actual_out = 0;
    const enum libdeflate_result res = libdeflate_zlib_decompress(
        d,
        compressed.data(),
        compressed.size(),
        decompressed.data(),
        decompressed.size(),
        &actual_out);
    libdeflate_free_decompressor(d);
    return res == LIBDEFLATE_SUCCESS;
#else
    z_stream stream{};
    stream.zalloc = Z_NULL;
    stream.zfree = Z_NULL;
    stream.opaque = Z_NULL;
    if (inflateInit(&stream) != Z_OK) return false;
    stream.avail_in = static_cast<uInt>(compressed.size());
    stream.next_in = reinterpret_cast<Bytef*>(compressed.data());
    stream.avail_out = static_cast<uInt>(decompressed.size());
    stream.next_out = reinterpret_cast<Bytef*>(decompressed.data());
    const int ret = inflate(&stream, Z_FINISH);
    inflateEnd(&stream);
    return ret == Z_STREAM_END;
#endif
}

u32 GetPFSCOffset(std::span<const u8> pfs_image) {
    // Little-endian "PFSC" on disk.
    static constexpr u32 PfscMagic = 0x43534650;
    u32 value = 0;
    // shadPS4 starts at 0x20000; also scan earlier in case layout differs.
    const u32 start = pfs_image.size() > 0x20000 ? 0x10000u : 0u;
    for (u32 i = start; i + 4 <= pfs_image.size(); i += 0x10000) {
        std::memcpy(&value, &pfs_image[i], sizeof(u32));
        if (value == PfscMagic) return i;
    }
    // Byte-scan fallback every 0x1000 for atypical images.
    for (u32 i = 0; i + 4 <= pfs_image.size(); i += 0x1000) {
        std::memcpy(&value, &pfs_image[i], sizeof(u32));
        if (value == PfscMagic) return i;
    }
    return static_cast<u32>(-1);
}

bool safe_under(const fs::path& root, const fs::path& child) {
    const auto r = fs::weakly_canonical(root);
    std::error_code ec;
    const auto c = fs::weakly_canonical(child, ec);
    if (ec) {
        // weakly_canonical may fail for non-existing; check lexically
        auto rel = child.lexically_relative(root);
        return !rel.empty() && rel.native().find("..") == std::string::npos;
    }
    auto rel = c.lexically_relative(r);
    return !rel.empty() && rel.native().find("..") == std::string::npos;
}

struct ExtractState {
    PKGHeader hdr{};
    Crypto crypto;
    std::array<u8, 32> ekpfsKey{};
    std::array<u8, 16> dataKey{};
    std::array<u8, 16> tweakKey{};
    std::vector<pfs_fs_table> fsTable;
    std::vector<Inode> iNodeBuf;
    std::vector<u64> sectorMap;
    u64 pfsc_offset = 0;
    std::unordered_map<int, fs::path> extractPaths;
    fs::path extract_root;
    fs::path current_dir;
    std::string content_id;
    std::string title_id;
};

bool read_header(int fd, PKGHeader& hdr) {
    return pread_all(fd, &hdr, sizeof(hdr), 0);
}

bool digests_equal(const std::array<u8, 32>& a, const std::array<u8, 32>& b) {
    return std::memcmp(a.data(), b.data(), 32) == 0;
}

bool derive_keys_from_entries(int fd, ExtractState& st, const char* passcode, std::string& err) {
    const u32 offset = st.hdr.pkg_table_entry_offset;
    const u32 n_files = st.hdr.pkg_table_entry_count;
    std::array<u8, 32> seed_digest{};
    std::array<std::array<u8, 32>, 7> digest1{};
    std::array<std::array<u8, 256>, 7> key1{};
    std::array<u8, 256> imgkeydata{};
    std::array<u8, 32> dk3{};
    std::array<u8, 32> ivKey{};
    std::array<u8, 256> imgKey{};
    bool have_entry_keys = false;
    bool have_image_key = false;
    bool dk3_ok = false;
    PKGEntry image_entry{};

    for (u32 i = 0; i < n_files; ++i) {
        PKGEntry entry{};
        if (!pread_all(fd, &entry, sizeof(entry), static_cast<off_t>(offset) + i * 32)) {
            err = "Failed reading entry table";
            return false;
        }
        const u32 id = u32(entry.id);
        if (id == 0x10) { // ENTRY_KEYS
            off_t pos = static_cast<off_t>(u32(entry.offset));
            if (!pread_all(fd, seed_digest.data(), 32, pos)) return false;
            pos += 32;
            for (int k = 0; k < 7; ++k) {
                if (!pread_all(fd, digest1[k].data(), 32, pos)) return false;
                pos += 32;
            }
            for (int k = 0; k < 7; ++k) {
                if (!pread_all(fd, key1[k].data(), 256, pos)) return false;
                pos += 256;
            }
            try {
                st.crypto.RSA2048Decrypt(dk3, key1[3], true);
                dk3_ok = true;
            } catch (const std::exception& ex) {
                LOGI("DK3 RSA decrypt failed (may be ok for passcode pkgs): %s", ex.what());
                dk3_ok = false;
            }
            have_entry_keys = true;
        } else if (id == 0x20) { // IMAGE_KEY
            if (!pread_all(fd, imgkeydata.data(), 256, static_cast<off_t>(u32(entry.offset)))) {
                return false;
            }
            image_entry = entry;
            have_image_key = true;
        }
    }

    if (!have_entry_keys) {
        err = "PKG missing ENTRY_KEYS";
        return false;
    }

    // Passcode path: MUST verify digest0 before accepting (LibOrbisPkg CheckPasscode).
    // Unverified ComputeKeys always "works" and previously poisoned EKPFS → PFSC not found.
    if (passcode && std::strlen(passcode) == 32 && st.content_id.size() == 36) {
        std::array<u8, 32> dk0{};
        std::array<u8, 32> digest0{};
        if (Crypto::ComputeKeys(st.content_id, passcode, 0, dk0)) {
            Crypto::XorSha256Digest(dk0, digest0);
            if (digests_equal(digest0, digest1[0])) {
                std::array<u8, 32> ek{};
                if (Crypto::ComputeKeys(st.content_id, passcode, 1, ek)) {
                    st.ekpfsKey = ek;
                    LOGI("Using verified passcode-derived EKPFS");
                    return true;
                }
            } else {
                LOGI("passcode digest mismatch (not accepting ComputeKeys)");
            }
        }
    }

    // Fake/homebrew PKG: RSA-decrypt IMAGE_KEY with FakeKeyset after DK3 unwrap.
    if (have_entry_keys && have_image_key && dk3_ok) {
        std::array<u8, 64> concat{};
        std::memcpy(concat.data(), &image_entry, sizeof(image_entry));
        std::memcpy(concat.data() + sizeof(image_entry), dk3.data(), 32);
        st.crypto.ivKeyHASH256(concat, ivKey);
        st.crypto.aesCbcCfb128Decrypt(ivKey, imgkeydata, imgKey);
        try {
            st.crypto.RSA2048Decrypt(st.ekpfsKey, imgKey, false);
            // Optional: verify derived key digest index 1 when present.
            std::array<u8, 32> ek_digest{};
            Crypto::XorSha256Digest(st.ekpfsKey, ek_digest);
            if (!digests_equal(ek_digest, digest1[1])) {
                LOGI("RSA EKPFS digest1 mismatch — still trying (some fakes differ)");
            }
            LOGI("Using fake-pkg EKPFS via RSA");
            return true;
        } catch (const std::exception& ex) {
            err = std::string("EKPFS RSA failed: ") + ex.what();
            LOGI("%s", err.c_str());
            // fall through to NEED_PASSCODE
        }
    }

    err = "NEED_PASSCODE";
    return false;
}

// Load one PFSC payload sector into `compressed_out`.
// Fast path: copy from the initial decrypted PFS cache window (pfs_cache_size*2).
// Slow path: when directory/inode blocks sit past that window (large titles like
// Bloodborne), stream+decrypt from the full PKG — same windowing as extract_file.
// Without this, build_fs_table used to `break` and silently produce a half tree
// (~15G of 25G for CUSA00900, missing sce_module/libc.prx → exit 133).
bool load_pfsc_sector(int fd, ExtractState& st, const std::vector<u8>& pfsc_cache,
                      u64 sector_offset, u64 sector_size,
                      std::vector<char>& compressed_out, std::string& err) {
    if (sector_size == 0 || sector_size > 0x20000) {
        err = "bad PFSC sector size";
        return false;
    }
    compressed_out.resize(static_cast<size_t>(sector_size));
    if (sector_offset + sector_size <= pfsc_cache.size()) {
        std::memcpy(compressed_out.data(),
                    pfsc_cache.data() + static_cast<size_t>(sector_offset),
                    static_cast<size_t>(sector_size));
        return true;
    }

    constexpr u64 kWindow = 0x11000;
    std::vector<u8> enc(kWindow);
    std::vector<u8> dec(kWindow);
    const u64 file_offset =
        u64(st.hdr.pfs_image_offset) + static_cast<u64>(st.pfsc_offset) + sector_offset;
    const u64 current_sector = (static_cast<u64>(st.pfsc_offset) + sector_offset) / 0x1000;
    const u64 aligned = (sector_offset + static_cast<u64>(st.pfsc_offset)) & ~u64{0xFFF};
    const int previous_data =
        static_cast<int>((sector_offset + static_cast<u64>(st.pfsc_offset)) - aligned);
    std::fill(enc.begin(), enc.end(), 0);
    const u64 read_off = file_offset - static_cast<u64>(previous_data);
    const u64 pkg_size = u64(st.hdr.pkg_size);
    const u64 max_read = read_off < pkg_size ? (pkg_size - read_off) : 0;
    const size_t to_read = static_cast<size_t>(std::min<u64>(kWindow, max_read));
    if (to_read > 0 && !pread_all(fd, enc.data(), to_read, static_cast<off_t>(read_off))) {
        err = "PFS metadata sector read failed";
        return false;
    }
    st.crypto.decryptPFS(st.dataKey, st.tweakKey, enc, dec, current_sector);
    if (static_cast<size_t>(previous_data) + sector_size > dec.size()) {
        err = "PFS metadata sector window OOB";
        return false;
    }
    std::memcpy(compressed_out.data(), dec.data() + previous_data, static_cast<size_t>(sector_size));
    return true;
}

bool build_fs_table(int fd, ExtractState& st, std::string& err) {
    std::array<u8, 16> seed{};
    if (!pread_all(fd, seed.data(), 16, static_cast<off_t>(u64(st.hdr.pfs_image_offset) + 0x370))) {
        err = "Failed to read PFS seed";
        return false;
    }
    const u64 pfs_flags = u64(st.hdr.pfs_image_flags);
    const bool new_crypt = (pfs_flags & 0x2000000000000000ULL) != 0;
    st.crypto.PfsGenCryptoKey(st.ekpfsKey, seed, st.dataKey, st.tweakKey, new_crypt);
    LOGI("PfsGenCryptoKey new_crypt=%d pfs_flags=0x%llx",
         new_crypt ? 1 : 0,
         static_cast<unsigned long long>(pfs_flags));
    // Match shadPS4: decrypt first pfs_cache_size*2 bytes of outer PFS image.
    u32 length = u32(st.hdr.pfs_cache_size) * 2;
    if (length == 0) {
        length = 0x100000;
        LOGI("pfs_cache_size=0, fallback length=0x%x", length);
    }
    if (length < 0x40000) length = 0x40000;
    const u64 pfs_off = u64(st.hdr.pfs_image_offset);
    const u64 pfs_sz = u64(st.hdr.pfs_image_size);
    if (pfs_sz > 0 && length > pfs_sz) length = static_cast<u32>(pfs_sz);
    // LibOrbisPkg: crypt starts at BlockSize/0x1000 (usually 16).
    constexpr uint64_t kCryptStartSector = 16;
    LOGI("build_fs_table pfs_off=%llu pfs_sz=%llu cache=%u length=%u cryptStart=%llu",
         static_cast<unsigned long long>(pfs_off),
         static_cast<unsigned long long>(pfs_sz),
         u32(st.hdr.pfs_cache_size),
         length,
         static_cast<unsigned long long>(kCryptStartSector));
    std::vector<u8> pfs_encrypted(length);
    std::vector<u8> pfs_decrypted(length);
    if (!pread_all(fd, pfs_encrypted.data(), length, static_cast<off_t>(pfs_off))) {
        err = "Failed reading PFS header region";
        return false;
    }
    auto try_find_pfsc = [&](bool use_new_crypt) -> bool {
        st.crypto.PfsGenCryptoKey(st.ekpfsKey, seed, st.dataKey, st.tweakKey, use_new_crypt);
        st.crypto.decryptPFS(st.dataKey, st.tweakKey, pfs_encrypted, pfs_decrypted, 0, kCryptStartSector);
        st.pfsc_offset = GetPFSCOffset(pfs_decrypted);
        return st.pfsc_offset != static_cast<u32>(-1) && st.pfsc_offset < length;
    };
    bool found = try_find_pfsc(new_crypt);
    if (!found && !new_crypt) {
        LOGI("PFSC miss with new_crypt=0, retry new_crypt=1");
        found = try_find_pfsc(true);
    }
    if (!found && new_crypt) {
        LOGI("PFSC miss with new_crypt=1, retry new_crypt=0");
        found = try_find_pfsc(false);
    }
    // Also try crypt_start_sector=0 (shadPS4 style) if still missing.
    if (!found) {
        LOGI("PFSC miss with cryptStart=16, retry cryptStart=0");
        st.crypto.PfsGenCryptoKey(st.ekpfsKey, seed, st.dataKey, st.tweakKey, new_crypt);
        st.crypto.decryptPFS(st.dataKey, st.tweakKey, pfs_encrypted, pfs_decrypted, 0, 0);
        st.pfsc_offset = GetPFSCOffset(pfs_decrypted);
        found = st.pfsc_offset != static_cast<u32>(-1) && st.pfsc_offset < length;
    }
    if (!found) {
        LOGI("PFSC not found; plain[0..15]=%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x",
             pfs_encrypted[0], pfs_encrypted[1], pfs_encrypted[2], pfs_encrypted[3],
             pfs_encrypted[4], pfs_encrypted[5], pfs_encrypted[6], pfs_encrypted[7],
             pfs_encrypted[8], pfs_encrypted[9], pfs_encrypted[10], pfs_encrypted[11],
             pfs_encrypted[12], pfs_encrypted[13], pfs_encrypted[14], pfs_encrypted[15]);
        LOGI("PFSC not found; decrypted[0..15]=%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x",
             pfs_decrypted[0], pfs_decrypted[1], pfs_decrypted[2], pfs_decrypted[3],
             pfs_decrypted[4], pfs_decrypted[5], pfs_decrypted[6], pfs_decrypted[7],
             pfs_decrypted[8], pfs_decrypted[9], pfs_decrypted[10], pfs_decrypted[11],
             pfs_decrypted[12], pfs_decrypted[13], pfs_decrypted[14], pfs_decrypted[15]);
        if (length > 0x20000) {
            LOGI("decrypted@0x20000=%02x%02x%02x%02x",
                 pfs_decrypted[0x20000], pfs_decrypted[0x20001],
                 pfs_decrypted[0x20002], pfs_decrypted[0x20003]);
        }
        err = "PFSC magic not found (wrong keys or unsupported PFS layout)";
        return false;
    }
    LOGI("PFSC offset=0x%llx", static_cast<unsigned long long>(st.pfsc_offset));
    if (st.pfsc_offset >= length || sizeof(PFSCHdr) > length - st.pfsc_offset) {
        err = "PFSC header OOB";
        return false;
    }
    std::vector<u8> pfsc(length - st.pfsc_offset);
    std::memcpy(pfsc.data(), pfs_decrypted.data() + st.pfsc_offset, pfsc.size());
    PFSCHdr pfsChdr{};
    std::memcpy(&pfsChdr, pfsc.data(), sizeof(pfsChdr));
    if (pfsChdr.block_sz2 <= 0 || pfsChdr.data_length <= 0) {
        err = "Invalid PFSC geometry";
        return false;
    }
    const int64_t num_blocks64 = pfsChdr.data_length / pfsChdr.block_sz2;
    // Cap: avoid huge allocs from corrupt headers.
    if (num_blocks64 <= 0 || num_blocks64 > 2'000'000) {
        err = "PFSC num_blocks out of range";
        LOGE("num_blocks=%lld data_length=%lld block_sz2=%lld",
             static_cast<long long>(num_blocks64),
             static_cast<long long>(pfsChdr.data_length),
             static_cast<long long>(pfsChdr.block_sz2));
        return false;
    }
    const int num_blocks = static_cast<int>(num_blocks64);
    const int64_t map_bytes = (static_cast<int64_t>(num_blocks) + 1) * 8;
    if (pfsChdr.block_offsets < 0 ||
        static_cast<uint64_t>(pfsChdr.block_offsets) + static_cast<uint64_t>(map_bytes) > pfsc.size()) {
        err = "PFSC sector map OOB";
        return false;
    }
    LOGI("PFSC num_blocks=%d block_offsets=%lld", num_blocks,
         static_cast<long long>(pfsChdr.block_offsets));
    st.sectorMap.resize(static_cast<size_t>(num_blocks) + 1);
    for (int i = 0; i < num_blocks + 1; ++i) {
        std::memcpy(&st.sectorMap[i],
                    pfsc.data() + pfsChdr.block_offsets + static_cast<size_t>(i) * 8, 8);
    }

    u32 ent_size = 0;
    u32 ndinode = 0;
    // flat_path_table uses its own index for extractPaths[…]=root. MUST NOT share
    // with the FILE/DIR listing counter — sharing made end_reached fire early on
    // large titles (Bloodborne: listed ~8500 of ~17129 inodes → half tree, no libc.prx).
    int flat_path_idx = 0;
    int listed_entries = 0;
    // PFSC also holds file-data blocks after the dirent run. Once dinode parsing
    // yields no FILE/DIR for a few blocks, further blocks are payload — stop.
    // Without this, BB (num_blocks≈414k) hangs in "Preparing extract" forever.
    int empty_dinode_blocks = 0;
    bool dinode_reached = false;
    bool uroot_reached = false;
    std::vector<char> compressedData;
    std::vector<char> decompressedData(0x10000);

    st.extractPaths[0] = st.extract_root;
    st.current_dir = st.extract_root;

    auto read_dirent = [&](int j, Dirent& dirent) -> bool {
        // Only 16-byte header is guaranteed; name length is bounded.
        if (j < 0 || j + 16 > 0x10000) return false;
        std::memcpy(&dirent.ino, &decompressedData[j], 4);
        std::memcpy(&dirent.type, &decompressedData[j + 4], 4);
        std::memcpy(&dirent.namelen, &decompressedData[j + 8], 4);
        std::memcpy(&dirent.entsize, &decompressedData[j + 12], 4);
        if (dirent.entsize <= 0 || j + dirent.entsize > 0x10000) return false;
        if (dirent.namelen < 0 || dirent.namelen >= 256) return false;
        if (16 + dirent.namelen > dirent.entsize) return false;
        std::memset(dirent.name, 0, sizeof(dirent.name));
        std::memcpy(dirent.name, &decompressedData[j + 16], static_cast<size_t>(dirent.namelen));
        return true;
    };

    int streamed_sectors = 0;
    for (int i = 0; i < num_blocks; ++i) {
        if (g_cancel.load()) {
            err = "CANCELLED";
            return false;
        }
        const u64 sectorOffset = st.sectorMap[static_cast<size_t>(i)];
        const u64 sectorSize = st.sectorMap[static_cast<size_t>(i) + 1] - sectorOffset;
        if (sectorSize == 0 || sectorSize > 0x20000) continue;
        // Beyond the initial cache window: stream from full PKG (do not break —
        // that silently truncated large titles mid-tree).
        if (sectorOffset + sectorSize > pfsc.size()) {
            ++streamed_sectors;
            if (streamed_sectors == 1) {
                LOGI("build_fs_table streaming past cache at block=%d off=0x%llx (cache=%zu)",
                     i, static_cast<unsigned long long>(sectorOffset), pfsc.size());
            }
        }
        if (!load_pfsc_sector(fd, st, pfsc, sectorOffset, sectorSize, compressedData, err)) {
            LOGE("build_fs_table sector %d: %s", i, err.c_str());
            return false;
        }
        std::fill(decompressedData.begin(), decompressedData.end(), 0);
        if (sectorSize == 0x10000) {
            std::memcpy(decompressedData.data(), compressedData.data(), 0x10000);
        } else if (sectorSize < 0x10000) {
            if (!DecompressPFSC(compressedData, decompressedData)) {
                err = "PFSC decompress failed (fs table)";
                return false;
            }
        } else {
            continue;
        }

        if (i == 0) {
            std::memcpy(&ndinode, decompressedData.data() + 0x30, 4);
            LOGI("ndinode=%u", ndinode);
            if (ndinode > 500000) {
                err = "ndinode unreasonable";
                return false;
            }
        }
        int occupied_blocks = static_cast<int>((ndinode * 0xA8) / 0x10000);
        if (((ndinode * 0xA8) % 0x10000) != 0) occupied_blocks += 1;

        if (i >= 1 && i <= occupied_blocks) {
            for (int p = 0; p + static_cast<int>(sizeof(Inode)) <= 0x10000; p += 0xA8) {
                Inode node{};
                std::memcpy(&node, &decompressedData[p], sizeof(node));
                if (node.Mode == 0) break;
                st.iNodeBuf.push_back(node);
            }
        }

        const std::string_view flat_path_table(&decompressedData[0x10], 15);
        if (flat_path_table == "flat_path_table") uroot_reached = true;

        if (uroot_reached) {
            for (int j = 0; j < 0x10000; ) {
                Dirent dirent{};
                if (!read_dirent(j, dirent)) break;
                ent_size = static_cast<u32>(dirent.entsize);
                if (ent_size == 0) break;
                if (dirent.ino != 0) {
                    flat_path_idx++;
                } else {
                    st.extractPaths[flat_path_idx] = st.extract_root;
                    uroot_reached = false;
                    break;
                }
                j += static_cast<int>(ent_size);
            }
        }

        const char dot = decompressedData[0x10];
        const std::string_view dotdot(&decompressedData[0x28], 2);
        if (dot == '.' && dotdot == "..") dinode_reached = true;

        bool end_reached = false;
        if (dinode_reached) {
            int dirents_this_block = 0;
            int added_this_block = 0;
            for (int j = 0; j < 0x10000; ) {
                Dirent dirent{};
                if (!read_dirent(j, dirent)) break;
                if (dirent.ino == 0) break;
                ent_size = static_cast<u32>(dirent.entsize);
                if (ent_size == 0) break;
                dirents_this_block++;
                pfs_fs_table table{};
                table.name = std::string(dirent.name, static_cast<size_t>(dirent.namelen));
                table.inode = static_cast<u32>(dirent.ino);
                table.type = static_cast<u32>(dirent.type);
                if (table.type == PFS_CURRENT_DIR) {
                    auto it = st.extractPaths.find(static_cast<int>(table.inode));
                    if (it != st.extractPaths.end()) st.current_dir = it->second;
                }
                st.extractPaths[static_cast<int>(table.inode)] =
                    st.current_dir / fs::path(table.name);
                if (table.type == PFS_FILE || table.type == PFS_DIR) {
                    if (table.type == PFS_DIR) {
                        const auto& p = st.extractPaths[static_cast<int>(table.inode)];
                        if (safe_under(st.extract_root, p)) fs::create_directories(p);
                    }
                    st.fsTable.push_back(table);
                    listed_entries++;
                    added_this_block++;
                    // Hard cap: enough FILE/DIR entries relative to inode count.
                    // (flat_path_idx intentionally excluded — see declaration.)
                    if (listed_entries + 1 >= static_cast<int>(ndinode)) end_reached = true;
                }
                j += static_cast<int>(ent_size);
            }
            if (dirents_this_block == 0) {
                empty_dinode_blocks++;
                // Contiguous dirent run ended; remaining PFSC blocks are file data.
                // Do not key off added_this_block==0: blocks with only "."/".." are valid.
                if (listed_entries > 0 && empty_dinode_blocks >= 2) end_reached = true;
            } else {
                empty_dinode_blocks = 0;
            }
            if ((i % 64) == 0 || end_reached) {
                LOGI("build_fs_table progress block=%d/%d listed=%d added=%d empty=%d stream=%d",
                     i, num_blocks, listed_entries, added_this_block, empty_dinode_blocks,
                     streamed_sectors);
            }
            if (end_reached) break;
        }
    }
    LOGI("build_fs_table done entries=%zu listed=%d inodes=%zu ndinode=%u "
         "flat_path_idx=%d streamed_sectors=%d",
         st.fsTable.size(), listed_entries, st.iNodeBuf.size(), ndinode, flat_path_idx,
         streamed_sectors);
    return true;
}

bool extract_file(int fd, ExtractState& st, const pfs_fs_table& table, std::string& err,
                  void (*progress)(void* ctx, uint64_t done, uint64_t total, const char* file),
                  void* progress_ctx, const std::atomic<uint64_t>& done_global,
                  uint64_t total_bytes) {
    if (table.type != PFS_FILE) return true;
    if (table.inode < 0 || static_cast<size_t>(table.inode) >= st.iNodeBuf.size()) {
        err = "Bad inode";
        return false;
    }
    const Inode& node = st.iNodeBuf[table.inode];
    const int sector_loc = node.loc;
    const int nblocks = node.Blocks;
    const int bsize = node.Size;
    auto path_it = st.extractPaths.find(table.inode);
    if (path_it == st.extractPaths.end()) return true;
    const fs::path out_path = path_it->second;
    if (!safe_under(st.extract_root, out_path)) {
        err = "Path escapes extract root";
        return false;
    }
    fs::create_directories(out_path.parent_path());
    std::ofstream out(out_path, std::ios::binary);
    if (!out) {
        err = "Cannot open output file";
        return false;
    }

    int size_decompressed = 0;
    uint64_t written = 0;
    std::vector<char> compressedData;
    std::vector<char> decompressedData(0x10000);
    constexpr u64 pfsc_buf_size = 0x11000;
    std::vector<u8> pfsc(pfsc_buf_size);
    std::vector<u8> pfs_decrypted(pfsc_buf_size);
    // Throttle JNI progress: every 32 blocks (~2 MiB) or last block.
    constexpr int kProgressEveryBlocks = 32;

    for (int j = 0; j < nblocks; ++j) {
        if (g_cancel.load()) {
            err = "CANCELLED";
            return false;
        }
        if (sector_loc + j + 1 >= static_cast<int>(st.sectorMap.size())) {
            err = "sector map OOB";
            return false;
        }
        const u64 sectorOffset = st.sectorMap[sector_loc + j];
        const u64 sectorSize = st.sectorMap[sector_loc + j + 1] - sectorOffset;
        const u64 fileOffset = u64(st.hdr.pfs_image_offset) + st.pfsc_offset + sectorOffset;
        const u64 currentSector1 = (st.pfsc_offset + sectorOffset) / 0x1000;
        const int sectorOffsetMask = static_cast<int>((sectorOffset + st.pfsc_offset) & 0xFFFFF000);
        const int previousData = static_cast<int>((sectorOffset + st.pfsc_offset) - sectorOffsetMask);

        // Tolerate short reads near the PFS/PKG end: the needed sector
        // (sectorSize bytes at offset previousData) is always within the
        // valid region, but the fixed 0x11000 window can overshoot the file
        // on the final blocks. decryptPFS only consumes complete 0x1000
        // sectors, so zero-filled tail bytes never reach real output.
        std::fill(pfsc.begin(), pfsc.end(), 0);
        const u64 readOff = fileOffset - previousData;
        const u64 pkgSize = u64(st.hdr.pkg_size);
        const u64 maxRead = readOff < pkgSize ? (pkgSize - readOff) : 0;
        const size_t toRead = static_cast<size_t>(std::min<u64>(pfsc_buf_size, maxRead));
        if (toRead > 0) {
            size_t got = 0;
            while (got < toRead) {
                const ssize_t r = pread(fd, pfsc.data() + got, toRead - got,
                                         static_cast<off_t>(readOff) + static_cast<off_t>(got));
                if (r <= 0) {
                    err = "PFS read failed";
                    return false;
                }
                got += static_cast<size_t>(r);
            }
        }
        st.crypto.decryptPFS(st.dataKey, st.tweakKey, pfsc, pfs_decrypted, currentSector1);
        compressedData.resize(static_cast<size_t>(sectorSize));
        std::memcpy(compressedData.data(), pfs_decrypted.data() + previousData, static_cast<size_t>(sectorSize));
        if (sectorSize == 0x10000) {
            std::memcpy(decompressedData.data(), compressedData.data(), 0x10000);
        } else if (sectorSize < 0x10000) {
            if (!DecompressPFSC(compressedData, decompressedData)) {
                err = "PFSC decompress failed";
                return false;
            }
        }
        size_decompressed += 0x10000;
        if (j < nblocks - 1) {
            out.write(decompressedData.data(), static_cast<std::streamsize>(decompressedData.size()));
            written += static_cast<uint64_t>(decompressedData.size());
        } else {
            const u32 write_size = static_cast<u32>(decompressedData.size() - (size_decompressed - bsize));
            out.write(decompressedData.data(), static_cast<std::streamsize>(write_size));
            written += write_size;
        }
        if (progress &&
            (j == 0 || j == nblocks - 1 || ((j + 1) % kProgressEveryBlocks) == 0)) {
            const uint64_t partial =
                written > static_cast<uint64_t>(bsize) ? static_cast<uint64_t>(bsize) : written;
            progress(progress_ctx, done_global.load(std::memory_order_relaxed) + partial,
                     total_bytes, table.name.c_str());
        }
    }
    return true;
}

// Write raw sce_sys entries from package table (param.sfo etc).
bool extract_sce_sys_entries(int fd, ExtractState& st, std::string& err) {
    const u32 offset = st.hdr.pkg_table_entry_offset;
    const u32 n_files = st.hdr.pkg_table_entry_count;
    fs::create_directories(st.extract_root / "sce_sys");
    for (u32 i = 0; i < n_files; ++i) {
        PKGEntry entry{};
        if (!pread_all(fd, &entry, sizeof(entry), static_cast<off_t>(offset) + i * 32)) {
            err = "entry read failed";
            return false;
        }
        const auto name = GetEntryNameByType(entry.id);
        if (name.empty()) continue;
        std::vector<u8> data(entry.size);
        if (entry.size > 0) {
            if (!pread_all(fd, data.data(), entry.size, entry.offset)) {
                err = "entry data read failed";
                return false;
            }
        }
        const fs::path out = st.extract_root / "sce_sys" / std::string(name);
        if (!safe_under(st.extract_root, out)) continue;
        std::ofstream f(out, std::ios::binary);
        f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    return true;
}

} // namespace

void bachata_pkg_cancel(void) { g_cancel.store(true); }

int bachata_pkg_probe(int fd, BachataPkgProbe* out) {
    LOGI("probe start fd=%d", fd);
    if (!out) return 3;
    std::memset(out, 0, sizeof(*out));
    g_cancel.store(false);
    PKGHeader hdr{};
    if (!read_header(fd, hdr) || u32(hdr.magic) != 0x7F434E54) {
        out->status = 3;
        std::snprintf(out->message, sizeof(out->message), "Invalid PKG header");
        LOGE("probe bad header magic");
        return 3;
    }
    char cid[0x30]{};
    std::memcpy(cid, hdr.pkg_content_id, 0x24);
    std::snprintf(out->content_id, sizeof(out->content_id), "%s", cid);
    out->package_size = u64(hdr.pkg_size);
    out->pfs_image_size = u64(hdr.pfs_image_size);
    // Title hint: last 9 of content after EP...
    if (std::strlen(cid) >= 16) {
        std::snprintf(out->title_hint, sizeof(out->title_hint), "%.9s", cid + 7);
    }
    LOGI("probe header ok contentId=%s pkgSize=%llu pfsSize=%llu",
         cid,
         static_cast<unsigned long long>(out->package_size),
         static_cast<unsigned long long>(out->pfs_image_size));
    // Auth probe with zero passcode + fake path
    ExtractState st;
    st.hdr = hdr;
    st.content_id = cid;
    std::string err;
    LOGI("probe try zero-passcode keys");
    if (derive_keys_from_entries(fd, st, "00000000000000000000000000000000", err)) {
        out->status = 0;
        LOGI("probe ok via zero-passcode");
        return 0;
    }
    if (err == "NEED_PASSCODE") {
        out->status = 1;
        std::snprintf(out->message, sizeof(out->message), "Passcode required");
        LOGI("probe need passcode (after zero)");
        return 1;
    }
    // try without passcode (RSA only)
    err.clear();
    LOGI("probe try RSA/EKPFS keys");
    if (derive_keys_from_entries(fd, st, nullptr, err)) {
        out->status = 0;
        LOGI("probe ok via RSA/EKPFS");
        return 0;
    }
    if (err == "NEED_PASSCODE") {
        out->status = 1;
        std::snprintf(out->message, sizeof(out->message), "Passcode required");
        LOGI("probe need passcode (after RSA)");
        return 1;
    }
    out->status = 3;
    std::snprintf(out->message, sizeof(out->message), "%s", err.c_str());
    LOGE("probe error: %s", err.c_str());
    return 3;
}

int bachata_pkg_extract(int fd, const char* out_path, const char* passcode_or_null,
                        void (*progress)(void* ctx, uint64_t done, uint64_t total, const char* file),
                        void* progress_ctx) {
    LOGI("extract start fd=%d out=%s hasPasscode=%d",
         fd,
         out_path ? out_path : "(null)",
         passcode_or_null && passcode_or_null[0] ? 1 : 0);
    g_cancel.store(false);
    if (!out_path) return 3;
    ExtractState st;
    if (!read_header(fd, st.hdr) || u32(st.hdr.magic) != 0x7F434E54) {
        LOGE("extract bad header");
        return 3;
    }
    char cid[0x30]{};
    std::memcpy(cid, st.hdr.pkg_content_id, 0x24);
    st.content_id = cid;
    if (std::strlen(cid) >= 16) st.title_id.assign(cid + 7, 9);
    st.extract_root = fs::path(out_path);
    fs::create_directories(st.extract_root);

    std::string err;
    const char* pass = passcode_or_null;
    // Try provided passcode, then zero, then RSA-only.
    bool keyed = false;
    if (pass && std::strlen(pass) == 32) {
        LOGI("extract derive keys with provided passcode");
        keyed = derive_keys_from_entries(fd, st, pass, err);
    }
    if (!keyed) {
        err.clear();
        LOGI("extract derive keys with zero passcode");
        keyed = derive_keys_from_entries(fd, st, "00000000000000000000000000000000", err);
    }
    if (!keyed) {
        err.clear();
        LOGI("extract derive keys via RSA/EKPFS");
        keyed = derive_keys_from_entries(fd, st, nullptr, err);
    }
    if (!keyed) {
        if (err == "NEED_PASSCODE") {
            LOGI("extract need passcode");
            return 1;
        }
        LOGE("key derive: %s", err.c_str());
        return 3;
    }
    LOGI("extract keys ok contentId=%s", cid);

    LOGI("extract sce_sys entries");
    if (!extract_sce_sys_entries(fd, st, err)) {
        LOGE("sce_sys: %s", err.c_str());
        return 3;
    }
    LOGI("extract build fs table");
    if (!build_fs_table(fd, st, err)) {
        if (err == "CANCELLED") return 2;
        LOGE("fs table: %s", err.c_str());
        return 3;
    }
    LOGI("extract fs table files=%zu inodes=%zu", st.fsTable.size(), st.iNodeBuf.size());

    uint64_t total = 0;
    for (const auto& t : st.fsTable) {
        if (t.type == PFS_FILE && t.inode >= 0 && static_cast<size_t>(t.inode) < st.iNodeBuf.size()) {
            total += static_cast<uint64_t>(st.iNodeBuf[t.inode].Size);
        }
    }
    LOGI("extract total_bytes=%llu", static_cast<unsigned long long>(total));
    // Immediate UI update before first large file (avoids stale "Copying…" for minutes).
    if (progress) {
        progress(progress_ctx, 0, total, "");
    }

    // Parallel per-file extraction. extract_file touches only read-only
    // ExtractState fields + its own ofstream; Crypto is stateless (no members);
    // pread on the shared fd is thread-safe (POSIX, no fd-offset mutation).
    // Safe to run across a worker pool. Progress accumulates via an atomic.
    std::vector<size_t> fileIdx;
    fileIdx.reserve(st.fsTable.size());
    for (size_t i = 0; i < st.fsTable.size(); ++i) {
        if (st.fsTable[i].type == PFS_FILE) fileIdx.push_back(i);
    }
    const size_t fileCount = fileIdx.size();
    if (fileCount == 0) {
        if (progress) progress(progress_ctx, 0, total, "");
        LOGI("extract done files=0 bytes=0");
        return 0;
    }

    std::atomic<uint64_t> done{0};
    std::atomic<size_t> next{0};
    std::atomic<bool> failed{false};
    std::mutex err_mu;
    std::string shared_err;
    // Cap at 8: diminishing returns past the big.LITTLE cluster, and each extra
    // thread is 8 MiB+ of scratch buffers (pfsc + decrypted + inflight I/O).
    // fileCount >= 1 here so clamp high bound is never below the low bound of 1.
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    const unsigned nthreads = std::clamp<unsigned>(
        hw, 1u, std::min<unsigned>(8u, static_cast<unsigned>(fileCount)));
    LOGI("extract parallel files=%zu threads=%u", fileCount, nthreads);

    auto worker = [&]() {
        while (!g_cancel.load() && !failed.load()) {
            const size_t i = next.fetch_add(1, std::memory_order_relaxed);
            if (i >= fileCount) break;
            const auto& t = st.fsTable[fileIdx[i]];
            std::string err;
            if (!extract_file(fd, st, t, err, progress, progress_ctx, done, total)) {
                failed.store(true, std::memory_order_relaxed);
                std::lock_guard<std::mutex> lk(err_mu);
                if (shared_err.empty()) shared_err = err;
                break;
            }
            if (t.inode >= 0 && static_cast<size_t>(t.inode) < st.iNodeBuf.size()) {
                done.fetch_add(static_cast<uint64_t>(st.iNodeBuf[t.inode].Size),
                               std::memory_order_relaxed);
            }
            if (progress) {
                progress(progress_ctx, done.load(std::memory_order_relaxed), total, t.name.c_str());
            }
        }
    };

    std::vector<std::thread> pool;
    pool.reserve(nthreads);
    for (unsigned k = 0; k < nthreads; ++k) pool.emplace_back(worker);
    for (auto& th : pool) th.join();

    const uint64_t done_final = done.load(std::memory_order_relaxed);
    if (g_cancel.load()) {
        LOGI("extract cancelled");
        return 2;
    }
    if (failed.load()) {
        err = shared_err;
        if (err == "CANCELLED") return 2;
        LOGE("parallel extract: %s", err.c_str());
        return 3;
    }
    if (progress) progress(progress_ctx, done_final, total, "");
    LOGI("extract done files=%zu bytes=%llu", fileCount, static_cast<unsigned long long>(done_final));
    return 0;
}

} // namespace bachata_pkg

// C ABI
extern "C" {

void bachata_pkg_cancel(void) { bachata_pkg::bachata_pkg_cancel(); }

int bachata_pkg_probe(int fd, BachataPkgProbe* out) { return bachata_pkg::bachata_pkg_probe(fd, out); }

int bachata_pkg_extract(int fd, const char* out_path, const char* passcode_or_null,
                        void (*progress)(void* ctx, uint64_t done, uint64_t total, const char* file),
                        void* progress_ctx) {
    return bachata_pkg::bachata_pkg_extract(fd, out_path, passcode_or_null, progress, progress_ctx);
}

}
