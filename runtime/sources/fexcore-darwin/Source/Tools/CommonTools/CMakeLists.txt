set(SRCS DummyHandlers.cpp)

if (NOT MINGW AND NOT APPLE)
  # ELFContainer.cpp parses Linux ELF binaries via <elf.h> (Elf64_auxv_t etc.), which
  # doesn't exist on Darwin at all -- it's glibc/Linux-format-specific, not a portability
  # gap. Only consumers are FEXInterpreter/AOTGenerator/LinuxSyscalls (full Linux-guest-
  # process loading), none of which are built in FEXCORE_ONLY scope.
  list(APPEND SRCS Linux/Utils/ELFContainer.cpp)
endif()

add_library(CommonTools STATIC ${SRCS})
target_link_libraries(CommonTools FEXCore_Base FEXHeaderUtils)
target_include_directories(CommonTools PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
