#!/usr/bin/env bash
# Builds aetherps4-pkg-extract, a small native arm64-apple-darwin CLI tool
# that decrypts/extracts a PS4 .pkg into a plain game directory shadPS4 can
# load via its existing `-g <path>` flag.
#
# This is the same PKG-extraction logic (crypto, PFS parsing, extraction)
# already used and tested by the Bachata Android app's native module at
# android/BachataS4/core/runtime/src/main/cpp/pkg/, ported here as a
# standalone desktop tool: the only real change is swapping the Android
# JNI/javax.crypto RSA bridge for one built on LibreSSL's BIGNUM modexp (see
# pkg_rsa_native.cpp), since there's no JVM on the desktop build. It is a
# separate tool from shadps4/AetherPS4 itself and from the launcher app --
# it never links against FEXCore or the C++ emulator core, and does not
# require reconfiguring or rebuilding the main CMake project.
#
# Requires the shadps4-macos-arm64 build to already exist (see PORTING.md at
# the repo root) -- it reuses that build's already-compiled LibreSSL static
# library rather than building LibreSSL again from scratch.
set -euo pipefail

cd "$(dirname "$0")"

REPO_ROOT="$(cd ../.. && pwd)"
LIBRESSL_INCLUDE="$REPO_ROOT/externals/libressl/include"
LIBCRYPTO="$REPO_ROOT/runtime/build/shadps4-macos-arm64/externals/libressl/crypto/libcrypto.a"

if [[ ! -f "$LIBCRYPTO" ]]; then
    echo "error: $LIBCRYPTO not found." >&2
    echo "Build shadps4-macos-arm64 first (see PORTING.md) -- this tool reuses its already-built LibreSSL." >&2
    exit 1
fi

echo "Compiling aes.c (C)..."
clang -std=c11 -O2 -c aes.c -o aes.o

echo "Compiling C++ sources..."
clang++ -std=c++20 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
    -I. -I"$LIBRESSL_INCLUDE" \
    -c pkg_extractor.cpp pkg_crypto.cpp pkg_type.cpp pkg_rsa_native.cpp main_cli.cpp

echo "Linking..."
clang++ -std=c++20 -O2 \
    pkg_extractor.o pkg_crypto.o pkg_type.o pkg_rsa_native.o main_cli.o aes.o \
    "$LIBCRYPTO" -lz \
    -o aetherps4-pkg-extract

rm -f ./*.o

echo ""
echo "Built: $(pwd)/aetherps4-pkg-extract"
file aetherps4-pkg-extract
