# SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

project(ImGui_Resources)

set(IMGUI_FONT_EMBED_EXECUTABLE "" CACHE FILEPATH
    "Native Dear ImGui font embed executable required when cross-compiling")

if (CMAKE_CROSSCOMPILING)
    if (IMGUI_FONT_EMBED_EXECUTABLE STREQUAL "")
        message(FATAL_ERROR
            "IMGUI_FONT_EMBED_EXECUTABLE must name a native executable when cross-compiling")
    endif()
    if (NOT IS_ABSOLUTE "${IMGUI_FONT_EMBED_EXECUTABLE}")
        message(FATAL_ERROR "IMGUI_FONT_EMBED_EXECUTABLE must be an absolute path")
    endif()
    if (NOT EXISTS "${IMGUI_FONT_EMBED_EXECUTABLE}")
        message(FATAL_ERROR "IMGUI_FONT_EMBED_EXECUTABLE does not exist")
    endif()
    set(IMGUI_FONT_EMBED_COMMAND "${IMGUI_FONT_EMBED_EXECUTABLE}")
    set(IMGUI_FONT_EMBED_DEPENDS)
else()
    add_executable(Dear_ImGui_FontEmbed
        ${CMAKE_SOURCE_DIR}/externals/dear_imgui/misc/fonts/binary_to_compressed_c.cpp)
    set(IMGUI_FONT_EMBED_COMMAND "$<TARGET_FILE:Dear_ImGui_FontEmbed>")
    set(IMGUI_FONT_EMBED_DEPENDS Dear_ImGui_FontEmbed)
endif()

set(FONT_LIST
    NotoSansArabic-Regular.ttf
    NotoSansCJK-Regular.ttc
    NotoSans-Regular.ttf
    NotoSansSymbols2-Regular.ttf
    NotoSansThai-Regular.ttf
    ProggyVector-Regular.ttf
)

set(OutputList "")
FOREACH (FONT_FILE ${FONT_LIST})
    string(REGEX REPLACE "-" "_" fontname ${FONT_FILE})
    string(TOLOWER ${fontname} fontname)
    string(REGEX REPLACE "\\.(ttf|otf|ttc)$" "" fontname_cpp ${fontname})
    set(fontname_cpp "imgui_font_${fontname_cpp}")

    MESSAGE(STATUS "Embedding font ${FONT_FILE}")
    set(OUTPUT "generated_fonts/imgui_fonts/${fontname}")
    add_custom_command(
        OUTPUT "${OUTPUT}.g.cpp"
        COMMAND ${CMAKE_COMMAND} -E make_directory "generated_fonts/imgui_fonts"
        COMMAND ${IMGUI_FONT_EMBED_COMMAND} -nostatic "${CMAKE_CURRENT_SOURCE_DIR}/fonts/${FONT_FILE}" ${fontname_cpp} > "${OUTPUT}.g.cpp"
        DEPENDS ${IMGUI_FONT_EMBED_DEPENDS} "fonts/${FONT_FILE}"
        USES_TERMINAL
    )
    list(APPEND OutputList "${OUTPUT}.g.cpp")
ENDFOREACH ()

add_library(ImGui_Resources STATIC ${OutputList})
set(IMGUI_RESOURCES_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/generated_fonts PARENT_SCOPE)
