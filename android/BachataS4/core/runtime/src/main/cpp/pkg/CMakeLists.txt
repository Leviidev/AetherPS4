add_library(bachata_pkg SHARED
    bachata_pkg_jni.cpp
    pkg_extractor.cpp
    pkg_crypto.cpp
    pkg_type.cpp
    aes.c
)

target_include_directories(bachata_pkg PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(bachata_pkg PRIVATE cxx_std_20)
target_compile_options(bachata_pkg PRIVATE -fexceptions -frtti)
# Enable the ARMv8 AES/PMULL crypto instructions for the hardware XTS path in
# pkg_crypto.cpp (guarded by __ARM_FEATURE_AES). arm64-v8a mandates these on
# every conforming CPU. Scoped to bachata_pkg so sibling libs are unaffected.
if(ANDROID_ABI STREQUAL "arm64-v8a")
    target_compile_options(bachata_pkg PRIVATE -march=armv8-a+crypto)
endif()
# Force -O2 on the PKG extract/crypto hot path regardless of CMake build type.
# AGP compiles native code at the clang default (-O0) for the debug variant;
# the XTS-AES / PFSC inflate / parallel extract loop is ~5-10x slower
# unoptimized — a debug APK imports a base-game PKG at ~200 MB/s instead of
# ~1-2 GB/s. The last -O flag wins, so this overrides the toolchain -O0.
# Mirrors the unconditional -O2 in winlator/CMakeLists.txt.
target_compile_options(bachata_pkg PRIVATE -O2)
find_library(log-lib log)
find_library(z-lib z)
target_link_libraries(bachata_pkg PRIVATE ${log-lib} ${z-lib})

# libdeflate: ~2x faster single-shot inflate than stock zlib, with a proper
# status return (the legacy zlib path discarded inflate()'s result). Static,
# decompression+zlib format only, compression/gzip/test programs disabled to
# shrink the build. Scoped to bachata_pkg; zlib kept as the #else fallback.
set(LIBDEFLATE_BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_COMPRESSION_SUPPORT OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_GZIP OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory(
    "${BACHATA_ROOT}/externals/libdeflate"
    "${CMAKE_CURRENT_BINARY_DIR}/libdeflate"
)
target_link_libraries(bachata_pkg PRIVATE libdeflate_static)
target_compile_definitions(bachata_pkg PRIVATE BACHATA_HAVE_LIBDEFLATE)
