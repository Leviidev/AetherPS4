// Native (non-Android) backend for the RSAES-PKCS1-v1_5 decrypt that
// pkg_crypto.cpp's Crypto::RSA2048Decrypt needs. On Android this went through
// a JNI bridge into javax.crypto because the hand-rolled bigint modexp in
// pkg_crypto.cpp is schoolbook-slow for 2048-bit CRT. Here we get the same
// speed from LibreSSL's BN_mod_exp (already vendored in externals/libressl
// for shadPS4's own use) without needing a JVM. The CRT algorithm and PKCS1
// unpadding are unchanged from the reference implementation.
#include "pkg_rsa_native.h"
#include "keys.h"

#include <openssl/bn.h>

#include <cstring>

namespace {

bool pkcs1_unpad(const uint8_t plain[256], uint8_t out_key[32]) {
    if (plain[0] != 0x00 || plain[1] != 0x02) return false;
    size_t i = 2;
    while (i < 256 && plain[i] != 0x00) ++i;
    if (i >= 256 || i < 10) return false;
    ++i;
    const size_t msg_len = 256 - i;
    if (msg_len < 32) {
        std::memset(out_key, 0, 32);
        std::memcpy(out_key + (32 - msg_len), plain + i, msg_len);
    } else {
        std::memcpy(out_key, plain + i, 32);
    }
    return true;
}

// RSA-CRT decrypt: m1 = c^dP mod p, m2 = c^dQ mod q, h = qInv*(m1-m2) mod p,
// m = m2 + h*q. Same formula as pkg_crypto.cpp's rsa_pkcs1_v15_decrypt_crt,
// but using LibreSSL's BIGNUM (Montgomery-accelerated) modexp instead of the
// hand-rolled schoolbook one.
template <typename Keyset>
bool rsa_crt_decrypt(const uint8_t cipher[256], uint8_t out_key[32]) {
    BN_CTX* ctx = BN_CTX_new();
    if (!ctx) return false;

    BIGNUM* c = BN_bin2bn(cipher, 256, nullptr);
    BIGNUM* p = BN_bin2bn(Keyset::Prime1, sizeof(Keyset::Prime1), nullptr);
    BIGNUM* q = BN_bin2bn(Keyset::Prime2, sizeof(Keyset::Prime2), nullptr);
    BIGNUM* dp = BN_bin2bn(Keyset::Exponent1, sizeof(Keyset::Exponent1), nullptr);
    BIGNUM* dq = BN_bin2bn(Keyset::Exponent2, sizeof(Keyset::Exponent2), nullptr);
    BIGNUM* qinv = BN_bin2bn(Keyset::Coefficient, sizeof(Keyset::Coefficient), nullptr);
    BIGNUM* n = BN_bin2bn(Keyset::Modulus, sizeof(Keyset::Modulus), nullptr);

    BIGNUM* cp = BN_new();
    BIGNUM* cq = BN_new();
    BIGNUM* m1 = BN_new();
    BIGNUM* m2 = BN_new();
    BIGNUM* diff = BN_new();
    BIGNUM* h = BN_new();
    BIGNUM* hq = BN_new();
    BIGNUM* m = BN_new();

    bool ok = c && p && q && dp && dq && qinv && n && cp && cq && m1 && m2 && diff && h && hq && m;
    if (ok) ok = BN_mod(cp, c, p, ctx) == 1;
    if (ok) ok = BN_mod(cq, c, q, ctx) == 1;
    if (ok) ok = BN_mod_exp(m1, cp, dp, p, ctx) == 1;
    if (ok) ok = BN_mod_exp(m2, cq, dq, q, ctx) == 1;
    if (ok) ok = BN_mod_sub(diff, m1, m2, p, ctx) == 1;
    if (ok) ok = BN_mod_mul(h, qinv, diff, p, ctx) == 1;
    if (ok) ok = BN_mul(hq, h, q, ctx) == 1;
    if (ok) ok = BN_add(m, m2, hq) == 1;
    if (ok) ok = BN_mod(m, m, n, ctx) == 1;

    uint8_t plain[256] = {0};
    if (ok) ok = BN_bn2binpad(m, plain, sizeof(plain)) >= 0;
    if (ok) ok = pkcs1_unpad(plain, out_key);

    BN_free(c);
    BN_free(p);
    BN_free(q);
    BN_free(dp);
    BN_free(dq);
    BN_free(qinv);
    BN_free(n);
    BN_free(cp);
    BN_free(cq);
    BN_free(m1);
    BN_free(m2);
    BN_free(diff);
    BN_free(h);
    BN_free(hq);
    BN_free(m);
    BN_CTX_free(ctx);
    return ok;
}

} // namespace

extern "C" int bachata_pkg_rsa_decrypt(const uint8_t* ciphertext256, uint8_t* out32, int is_dk3) {
    const bool ok = is_dk3 ? rsa_crt_decrypt<PkgDerivedKey3Keyset>(ciphertext256, out32)
                           : rsa_crt_decrypt<FakeKeyset>(ciphertext256, out32);
    return ok ? 0 : 1;
}
