# SPDX-License-Identifier: GPL-2.0-or-later
# Psi-Ops: The Mindgate Conspiracy (Original Xbox, Title ID 0x4D570016), statically recompiled

cmake_minimum_required(VERSION 3.22)
project(PsiOps VERSION 1.2.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_definitions(
    PSIOPS_VERSION_STR="${PROJECT_VERSION}"
    PSIOPS_CMAKE_VER_MAJOR=${PROJECT_VERSION_MAJOR}
    PSIOPS_CMAKE_VER_MINOR=${PROJECT_VERSION_MINOR}
    PSIOPS_CMAKE_VER_PATCH=${PROJECT_VERSION_PATCH}
)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
endif()

# Explicit optimized Release flags even when configured without a preset (release scripts, IDEs) or from a
# half-initialized CMakeCache.txt whose per-config Clang flags defaulted to ""
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT "x${CMAKE_CXX_COMPILER_FRONTEND_VARIANT}" STREQUAL "xMSVC")
    foreach(_lang IN ITEMS C CXX)
        if("${CMAKE_${_lang}_FLAGS_RELEASE}" STREQUAL "")
            set(CMAKE_${_lang}_FLAGS_RELEASE "-O3 -DNDEBUG" CACHE STRING "Flags used by the ${_lang} compiler during RELEASE builds." FORCE)
        endif()
    endforeach()
endif()
# NDEBUG on every target (incl. FetchContent subprojects) in optimized configs; asserts stay in Debug
add_compile_definitions($<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:NDEBUG>)

# Reproducible, path-private binaries: remap every host prefix (workspace, build tree, FetchContent sources,
# temp and user profile; forward/backslash and drive-letter-case variants) BEFORE the SDK runs
# FetchContent_MakeAvailable(), so no personal path reaches __FILE__, DWARF/CodeView debug info or PE RSDS records.
function(_psiops_prefix_variants out_var raw_path)
    set(_variants "")
    if(NOT "${raw_path}" STREQUAL "")
        file(TO_CMAKE_PATH "${raw_path}" _norm)
        set(_cands "${_norm}")
        if(EXISTS "${raw_path}")
            get_filename_component(_real "${raw_path}" REALPATH)
            file(TO_CMAKE_PATH "${_real}" _real_norm)
            list(APPEND _cands "${_real_norm}")
        endif()
        foreach(_p IN LISTS _cands)
            string(REGEX REPLACE "[/\\\\]+$" "" _p "${_p}")
            string(LENGTH "${_p}" _plen)
            # never remap a bare root/drive, and skip paths that would break the flag list
            if(_plen GREATER 3 AND NOT "${_p}" MATCHES "[,;]")
                list(APPEND _variants "${_p}")
                if("${_p}" MATCHES "^([A-Za-z]):(.*)$")
                    string(TOUPPER "${CMAKE_MATCH_1}" _drv_up)
                    string(TOLOWER "${CMAKE_MATCH_1}" _drv_lo)
                    set(_rest "${CMAKE_MATCH_2}")
                    string(REPLACE "/" "\\" _rest_bs "${_rest}")
                    list(APPEND _variants "${_drv_up}:${_rest}" "${_drv_up}:${_rest_bs}"
                                          "${_drv_lo}:${_rest}" "${_drv_lo}:${_rest_bs}")
                endif()
            endif()
        endforeach()
        list(REMOVE_DUPLICATES _variants)
    endif()
    set(${out_var} "${_variants}" PARENT_SCOPE)
endfunction()

# The first registration of a prefix wins its replacement, so register the most specific directories first
set(_psiops_seen_prefixes "")
set(_psiops_keyed_maps "")
function(_psiops_register_prefix raw_path replacement)
    _psiops_prefix_variants(_pfx_list "${raw_path}")
    set(_seen "${_psiops_seen_prefixes}")
    set(_maps "${_psiops_keyed_maps}")
    foreach(_old IN LISTS _pfx_list)
        list(FIND _seen "${_old}" _idx)
        if(_idx EQUAL -1)
            list(APPEND _seen "${_old}")
            string(LENGTH "${_old}" _olen)
            math(EXPR _olen_pad "100000 + ${_olen}")
            string(SUBSTRING "${_olen_pad}" 1 5 _olen_key)
            list(APPEND _maps "${_olen_key}|${_old}=${replacement}")
        endif()
    endforeach()
    set(_psiops_seen_prefixes "${_seen}" PARENT_SCOPE)
    set(_psiops_keyed_maps "${_maps}" PARENT_SCOPE)
endfunction()

set(_psiops_deps_root "${CMAKE_BINARY_DIR}/_deps")
if(DEFINED FETCHCONTENT_BASE_DIR AND NOT "${FETCHCONTENT_BASE_DIR}" STREQUAL "")
    set(_psiops_deps_root "${FETCHCONTENT_BASE_DIR}")
endif()
foreach(_dep IN ITEMS zydis tomlplusplus imgui SDL3)
    string(TOUPPER "${_dep}" _dep_up)
    string(TOLOWER "${_dep}" _dep_lo)
    _psiops_register_prefix("${FETCHCONTENT_SOURCE_DIR_${_dep_up}}" "deps/${_dep_lo}")
    _psiops_register_prefix("${_psiops_deps_root}/${_dep_lo}-src" "deps/${_dep_lo}")
endforeach()
_psiops_register_prefix("${_psiops_deps_root}" "deps")
_psiops_register_prefix("${CMAKE_BINARY_DIR}" "build")
_psiops_register_prefix("${CMAKE_SOURCE_DIR}" ".")
_psiops_register_prefix("$ENV{TMP}" "tmp")
_psiops_register_prefix("$ENV{TEMP}" "tmp")
_psiops_register_prefix("$ENV{TMPDIR}" "tmp")
_psiops_register_prefix("$ENV{USERPROFILE}" "user")
_psiops_register_prefix("$ENV{HOME}" "user")
# Shortest prefix first: newer Clang applies the LAST matching -ffile-prefix-map (GCC semantics), Clang 14 the longest
list(SORT _psiops_keyed_maps)

include(CheckCXXCompilerFlag)
set(_psiops_saved_try_type "${CMAKE_TRY_COMPILE_TARGET_TYPE}")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT "x${CMAKE_CXX_COMPILER_FRONTEND_VARIANT}" STREQUAL "xMSVC")
    check_cxx_compiler_flag("-ffile-prefix-map=/a=/b" PSIOPS_HAS_FILE_PREFIX_MAP)
    check_cxx_compiler_flag("-ffile-compilation-dir=." PSIOPS_HAS_FILE_COMPILATION_DIR)
    check_cxx_compiler_flag("-fdebug-compilation-dir=." PSIOPS_HAS_DEBUG_COMPILATION_DIR)
    check_cxx_compiler_flag("-gno-record-command-line" PSIOPS_HAS_GNO_RECORD_CMDLINE)
    if(PSIOPS_HAS_FILE_COMPILATION_DIR)
        add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-ffile-compilation-dir=.>)
    elseif(PSIOPS_HAS_DEBUG_COMPILATION_DIR)
        add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-fdebug-compilation-dir=.>)
    endif()
    if(PSIOPS_HAS_GNO_RECORD_CMDLINE)
        add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-gno-record-command-line>)
    endif()
    foreach(_entry IN LISTS _psiops_keyed_maps)
        string(SUBSTRING "${_entry}" 6 -1 _mapping)
        if(PSIOPS_HAS_FILE_PREFIX_MAP)
            add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-ffile-prefix-map=${_mapping}>")
        else()
            add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-fmacro-prefix-map=${_mapping}>"
                                "$<$<COMPILE_LANGUAGE:C,CXX>:-fdebug-prefix-map=${_mapping}>")
        endif()
    endforeach()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # clang-cl
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/Brepro>)
    foreach(_entry IN LISTS _psiops_keyed_maps)
        string(SUBSTRING "${_entry}" 6 -1 _mapping)
        add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-clang:-ffile-prefix-map=${_mapping}>")
    endforeach()
elseif(MSVC)
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/Brepro>)
    foreach(_old IN LISTS _psiops_seen_prefixes)
        if("${_old}" MATCHES "\\\\")
            add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/d1trimfile:${_old}\\>")
        endif()
    endforeach()
endif()
set(CMAKE_TRY_COMPILE_TARGET_TYPE "${_psiops_saved_try_type}")

# Deterministic PE timestamps + bare PDB file name in RSDS records (Win64); content-hash build IDs and no debug
# sections in Release ELF (Linux). Applies to PsiOps, PsiOps_code, xbe_recomp and xbe_symbols alike.
# build.bat / release.bat clear _PDB so cmd.exe-wrapped Ninja link steps keep %_PDB% literal for lld-link.
if(WIN32 AND (MSVC OR "x${CMAKE_CXX_SIMULATE_ID}" STREQUAL "xMSVC"))
    add_link_options("LINKER:/Brepro" "LINKER:/PDBALTPATH:%_PDB%")
elseif(UNIX AND NOT APPLE)
    add_link_options("LINKER:--build-id=sha1" "$<$<CONFIG:Release>:LINKER:--strip-debug>")
endif()

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
    target_link_libraries(PsiOps PRIVATE wintrust crypt32 advapi32 oldnames winhttp shell32)
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
