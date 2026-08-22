# Bachata Vortek Android/Bionic server.
# Mali freeflight / present / suballoc work lives in the vendored tree under
# vendor/ (copied from runtime/sources/winlator-app so parent git tracks it).
# bachata_xwindow_swapchain.c remains the Bachata present ownership overlay.

get_filename_component(BACHATA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../../../" ABSOLUTE)
set(VORTEK_VENDOR "${CMAKE_CURRENT_SOURCE_DIR}/vendor")
# Optional pristine upstream for headers not yet vendored (winlator helpers only).
set(WINLATOR_UPSTREAM "${BACHATA_ROOT}/runtime/sources/winlator-app/app/src/main/cpp/winlator")

if(NOT EXISTS "${VORTEK_VENDOR}/src/request_handler.c")
    message(FATAL_ERROR "Vortek vendor sources missing at ${VORTEK_VENDOR}")
endif()
if(NOT EXISTS "${VORTEK_VENDOR}/src/vortek_gpu_track.c")
    message(FATAL_ERROR "vortek_gpu_track.c missing at ${VORTEK_VENDOR}/src")
endif()

add_library(bachata_vortek_server SHARED
    bachata_vortek_server.cpp
    bachata_vortek_jni.cpp
    bachata_vortek_io.c
    bachata_vk_context.c
    bachata_ahb_present.c
    bachata_vortek_stubs.c
    # Tracked Mali/Vortek fork (vendor/)
    ${VORTEK_VENDOR}/src/request_handler.c
    ${VORTEK_VENDOR}/src/vulkan_helper.c
    ${VORTEK_VENDOR}/src/vk_object.c
    ${VORTEK_VENDOR}/src/resource_memory.c
    ${VORTEK_VENDOR}/src/vortek_gpu_track.c
    ${VORTEK_VENDOR}/src/shader_inspector.c
    bachata_xwindow_swapchain.c
    ${VORTEK_VENDOR}/src/timeline_semaphore.c
    ${VORTEK_VENDOR}/src/async_pipeline_creator.c
    # Shared helpers (still from vendored winlator components when present)
    ../winlator/src/ring_buffer.c
    ../winlator/src/sysvshared_memory.c
    ../winlator/src/arrays.c
)

target_include_directories(bachata_vortek_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../winlator/include
    ${VORTEK_VENDOR}/include
    ${VORTEK_VENDOR}/src
    ${WINLATOR_UPSTREAM}/include
)

target_compile_options(bachata_vortek_server PRIVATE
    -Wall
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-implicit-function-declaration
    -Wno-incompatible-pointer-types-discards-qualifiers
    -fvisibility=default
    # events.h (via ring_buffer) uses write/read/usleep without includes
    -include unistd.h
)

target_compile_definitions(bachata_vortek_server PRIVATE
    BACHATA_VORTEK_SERVER=1
    VT_SERVER=1
    VK_NO_PROTOTYPES=1
    VK_USE_PLATFORM_ANDROID_KHR=1
)

target_link_libraries(bachata_vortek_server
    PRIVATE
    android
    dl
    log
    nativewindow
    jnigraphics
    EGL
    GLESv2
)
