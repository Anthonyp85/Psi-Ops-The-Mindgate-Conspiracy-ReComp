# SPDX-License-Identifier: GPL-2.0-or-later
# Psi-Ops: The Mindgate Conspiracy (Original Xbox, Title ID 0x4D570016), statically recompiled

cmake_minimum_required(VERSION 3.22)
project(PsiOps LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(PSIOPS_ALLOW_NO_XBE "Build host runtime and --headers-only targets without requiring default.xbe" OFF)

# SDK checkout at repository root (pinned via setup_sdk.{bat,sh} + patches/xboxxberecomp.patch)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/XboxXBERecomp/cmake/XbeRecomp.cmake")
    set(SDK "${CMAKE_CURRENT_SOURCE_DIR}/XboxXBERecomp")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sdk/cmake/XbeRecomp.cmake")
    set(SDK "${CMAKE_CURRENT_SOURCE_DIR}/sdk")
else()
    message(FATAL_ERROR "XboxXBERecomp SDK not found. Run setup_sdk.bat (Windows) or ./setup_sdk.sh (Linux) first.")
endif()

# Expected SHA-1 of retail USA default.xbe (used in --headers-only / copyright-clean mode)
set(XBE_EXPECTED_SHA1 "42dd95fa444039b82f843ff02a31833da97d41cf")

# Resolve local default.xbe path (.local/xbe/default.xbe preferred over assets/default.xbe)
set(PSIOPS_XBE_PATH "default.xbe")
if(PSIOPS_ALLOW_NO_XBE)
    set(XBE_NO_IMAGE ON)
    message(STATUS "PSIOPS_ALLOW_NO_XBE=ON: enabling XBE_NO_IMAGE (--headers-only mode)")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.local/xbe/default.xbe")
    set(PSIOPS_XBE_PATH ".local/xbe/default.xbe")
    message(STATUS "Using private extracted XBE: ${PSIOPS_XBE_PATH}")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/default.xbe")
    set(PSIOPS_XBE_PATH "assets/default.xbe")
    message(STATUS "Using assets XBE: ${PSIOPS_XBE_PATH}")
else()
    set(XBE_NO_IMAGE ON)
    message(STATUS "No local default.xbe found; defaulting to XBE_NO_IMAGE (--headers-only mode). Run tools/extract_xbe.py to extract privately.")
endif()

include("${SDK}/cmake/XbeRecomp.cmake")

file(GLOB XBE_HOST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*/*.cpp")
list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/game/")

# Partition count tracked in config/game.toml (default 128 for ~4.76 MiB across 9 executable sections)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/config/game.toml")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/config/game.toml")
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/config/game.toml" GAME_PARTITIONS REGEX "^partitions *=")
    string(REGEX MATCH "[0-9]+" GAME_PARTITIONS "${GAME_PARTITIONS}")
endif()
if(NOT GAME_PARTITIONS)
    set(GAME_PARTITIONS 128)
endif()

add_xbe_image(PsiOps "${PSIOPS_XBE_PATH}" game.toml ${GAME_PARTITIONS} game)

if(WIN32)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/psiops.rc")
        set(PSIOPS_RES "${CMAKE_CURRENT_BINARY_DIR}/psiops.res")
        add_custom_command(
            OUTPUT "${PSIOPS_RES}"
            COMMAND "${XBE_LLVM_BIN}/llvm-rc.exe" /I "${CMAKE_CURRENT_SOURCE_DIR}/resources" /FO "${PSIOPS_RES}" "${CMAKE_CURRENT_SOURCE_DIR}/resources/psiops.rc"
            DEPENDS
                "${CMAKE_CURRENT_SOURCE_DIR}/resources/psiops.rc"
                "${CMAKE_CURRENT_SOURCE_DIR}/resources/psiops.ico"
                "${CMAKE_CURRENT_SOURCE_DIR}/resources/app.manifest"
            COMMENT "Compiling Win32 resources (psiops.rc -> psiops.res)"
            VERBATIM)
        add_custom_target(psiops_resources DEPENDS "${PSIOPS_RES}")
        add_dependencies(PsiOps psiops_resources)
        string(REPLACE "-Xlinker /MANIFEST:EMBED" "-Xlinker /MANIFEST:NO" CMAKE_CXX_LINK_EXECUTABLE "${CMAKE_CXX_LINK_EXECUTABLE}")
        target_link_options(PsiOps PRIVATE "-Wl,/MANIFEST:NO" "${PSIOPS_RES}")
    endif()
    target_link_libraries(PsiOps PRIVATE wintrust crypt32 advapi32 oldnames)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/psiops.png")
        add_custom_command(TARGET PsiOps POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/resources/psiops.png"
                "$<TARGET_FILE_DIR:PsiOps>/psiops.png")
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/plugins")
        add_custom_command(TARGET PsiOps POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:PsiOps>/plugins"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/plugins"
                "$<TARGET_FILE_DIR:PsiOps>/plugins")
    endif()
else()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/psiops.png")
        add_custom_command(TARGET PsiOps POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/resources/psiops.png"
                "$<TARGET_FILE_DIR:PsiOps>/psiops.png")
    endif()
endif()
