add_subdirectory(cpp-optparse/)

set(NAME Common)
set(SRCS
  Config.cpp
  CPUInfo.cpp
  ArgumentLoader.cpp
  HostFeatures.cpp
  JSONPool.cpp
  SHMStats.cpp
  VolatileMetadata.cpp)

if (NOT MINGW)
  list(APPEND SRCS
    FileFormatCheck.cpp)
endif()

if (NOT MINGW AND NOT APPLE)
  # FEXServerClient.cpp: async IPC client for the out-of-process FEXServer daemon
  # (Linux epoll/ppoll-based reactor, no Darwin port -- and nothing in the
  # FEXCORE_ONLY scope talks to FEXServer, only the full FEXServer/FEXInterpreter/
  # TestHarnessRunner/Steam tools do, none of which are built here).
  # Linux/SBRKAllocations.cpp: sbrk()-based allocation tracking, Linux-specific
  # (MAP_FIXED_NOREPLACE has no Darwin equivalent), same no-consumer-in-scope story.
  list(APPEND SRCS
    FEXServerClient.cpp
    Linux/SBRKAllocations.cpp)
endif()

add_library(${NAME} STATIC ${SRCS})
target_link_libraries(${NAME} FEXCore_Base cpp-optparse tiny-json::tiny-json FEXHeaderUtils range-v3::range-v3)
target_include_directories(${NAME} PRIVATE ${CMAKE_BINARY_DIR}/generated)

set_target_properties(${NAME} PROPERTIES
  C_VISIBILITY_PRESET hidden
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN TRUE)
