// ARMv8 crypto-instruction AES-128 single-block primitives.
// Enabled when the build target exposes __ARM_FEATURE_AES (arm64-v8a mandates
// the AES instructions, so every conforming CPU has them).
//
// Used to accelerate the hot PFS XTS decryption loop in pkg_crypto.cpp.
// Round-key expansion reuses tiny-AES-c's portable KeyExpansion (the schedule
// is computed once per PKG, not per block, so its cost is negligible); only
// the per-block SubBytes/ShiftRows/MixColumns/AddRoundKey run on the hardware
// AES unit. Output is byte-identical to the tiny-AES-c reference path.
//
// Instruction semantics (ARMv8 ARM C3.10.22):
//   AESE(v, k)  = SubBytes(ShiftRows(v ^ k))     // AddRoundKey folded in
//   AESMC(v)    = MixColumns(v)
//   AESD(v, k)  = InvSubBytes(InvShiftRows(v ^ k))
//   AESIMC(v)   = InvMixColumns(v)
//
// Encrypt (equivalent form): for r in 0..8: state=AESMC(AESE(state,rk[r]));
//   state=AESE(state,rk[9]); state ^= rk[10].
// Decrypt mirrors it but needs the equivalent inverse round keys: the middle
// keys must have InvMixColumns applied (see FIPS-197 §5.3.5 / the AESD/AESIMC
// ordering note). We precompute those once per PKG.
#ifndef BACHATA_PKG_AES_NEON_H
#define BACHATA_PKG_AES_NEON_H

#if defined(__ARM_FEATURE_AES)
#include <arm_neon.h>
#include <cstdint>

namespace bachata_pkg {

// AES-128 expanded round keys (11 round keys of 16 bytes each).
struct Aes128Keys {
    uint8x16_t rk[11];
};

// Build the NEON round-key set from a tiny-AES-c schedule (176 bytes, the
// AES_ctx::RoundKey layout produced by AES_init_ctx). tiny-AES-c stores the
// schedule as 11 contiguous 16-byte round keys in order round 0..10.
inline Aes128Keys aes128_keys_from_schedule(const uint8_t round_keys[176]) {
    Aes128Keys k;
    for (int i = 0; i < 11; ++i) {
        k.rk[i] = vld1q_u8(round_keys + i * 16);
    }
    return k;
}

// Equivalent inverse round keys for AESD-based decryption (FIPS-197 §5.3.5):
// irk[0]   = rk[10]                 (final forward round key, reversed to front)
// irk[1..9]= AESIMC(rk[9..1])       (InvMixColumns applied to middle keys)
// irk[10]  = rk[0]                  (initial forward round key, reversed to back)
inline Aes128Keys aes128_inverse_keys(const Aes128Keys& fwd) {
    Aes128Keys inv;
    inv.rk[0] = fwd.rk[10];
    for (int i = 1; i < 10; ++i) {
        inv.rk[i] = vaesimcq_u8(fwd.rk[10 - i]);
    }
    inv.rk[10] = fwd.rk[0];
    return inv;
}

// AES-128 encrypt one 16-byte block (ECB).
inline uint8x16_t aes128_encrypt_block(const Aes128Keys& k, uint8x16_t v) {
    // Rounds 0..8: AESE (AddRoundKey + SubBytes + ShiftRows) then AESMC (MixColumns).
    for (int i = 0; i < 9; ++i) {
        v = vaesmcq_u8(vaeseq_u8(v, k.rk[i]));
    }
    // Round 9: AESE without MixColumns, then final AddRoundKey (round 10).
    v = vaeseq_u8(v, k.rk[9]);
    return veorq_u8(v, k.rk[10]);
}

// AES-128 decrypt one 16-byte block (ECB), using equivalent inverse keys.
inline uint8x16_t aes128_decrypt_block(const Aes128Keys& inv, uint8x16_t v) {
    // Rounds 0..8: AESD (AddRoundKey + InvSubBytes + InvShiftRows) then AESIMC.
    for (int i = 0; i < 9; ++i) {
        v = vaesimcq_u8(vaesdq_u8(v, inv.rk[i]));
    }
    // Round 9: AESD without InvMixColumns, then final AddRoundKey.
    v = vaesdq_u8(v, inv.rk[9]);
    return veorq_u8(v, inv.rk[10]);
}

} // namespace bachata_pkg

#endif // __ARM_FEATURE_AES
#endif // BACHATA_PKG_AES_NEON_H
