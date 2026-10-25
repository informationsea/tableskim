SET(FLTK_VERSION 1.4.5)
SET(DUCKDB_VERSION 1.5.6)

include(FetchContent)

# Every dependency is SYSTEM, so that its headers are system headers and their
# warnings stay out of TableSkim's own (tableskim-warnings).
#
# Git dependencies are cloned at their tag only (GIT_SHALLOW): the full history
# of DuckDB alone is close to 1 GB, and CI clones every dependency on each run.
# Archives are checked against their SHA-256 (URL_HASH); update the hash
# together with the URL.

# Build third-party dependencies as Release even when TableSkim itself is a
# Debug build. Single-config generators pick up CMAKE_BUILD_TYPE per directory,
# so overriding it here affects only the subprojects added below.
# MSVC is excluded: mixing Debug (/MDd) and Release (/MD) runtimes fails to link.
if(NOT MSVC AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

# System FLTK is supported on Linux only; macOS and Windows always bundle it.
# Without the GUI, FLTK is not needed at all.
if(UNIX AND NOT APPLE AND TABLESKIM_BUILD_GUI)
    option(TABLESKIM_USE_SYSTEM_FLTK "Use the system FLTK (>= 1.4) instead of building the bundled copy" OFF)
    option(TABLESKIM_X11_ONLY "Build the bundled FLTK for X11 only, without Wayland" OFF)
else()
    set(TABLESKIM_USE_SYSTEM_FLTK OFF)
    set(TABLESKIM_X11_ONLY OFF)
endif()
if(TABLESKIM_X11_ONLY AND TABLESKIM_USE_SYSTEM_FLTK)
    message(WARNING "TABLESKIM_X11_ONLY has no effect on the system FLTK")
endif()

if(TABLESKIM_USE_SYSTEM_FLTK)
    # FLTK 1.4 installs a CMake config package that provides the fltk::fltk and
    # fltk::images targets and the fltk_wrap_ui() function.
    find_package(FLTK 1.4 CONFIG REQUIRED)
elseif(TABLESKIM_BUILD_GUI)
    FetchContent_Declare(
        fltk
        SYSTEM
        GIT_REPOSITORY https://github.com/fltk/fltk.git
        GIT_TAG release-${FLTK_VERSION}
        GIT_SHALLOW TRUE
    )

    set(FLTK_USE_SYSTEM_LIBJPEG OFF)
    set(FLTK_USE_SYSTEM_LIBPNG OFF)
    set(FLTK_USE_SYSTEM_ZLIB OFF)
    set(FLTK_BUILD_TEST OFF)
    set(FLTK_BUILD_GL OFF)
    set(FLTK_BUILD_FORMS OFF)
    set(FLTK_BUILD_EXAMPLES OFF)
    set(FLTK_BUILD_SHARED_LIBS OFF)
    if(TABLESKIM_X11_ONLY)
        # For older distributions: no libwayland, libxkbcommon, libdecor or
        # D-Bus at run time. Cairo (with Pango) still draws, as in the X11
        # part of the default hybrid build, so text looks the same.
        set(FLTK_BACKEND_WAYLAND OFF)
        set(FLTK_GRAPHICS_CAIRO ON)
    endif()

    FetchContent_MakeAvailable(fltk)
endif()

FetchContent_Declare(
    duckdb
    SYSTEM
    GIT_REPOSITORY https://github.com/duckdb/duckdb.git
    GIT_TAG v${DUCKDB_VERSION}
    GIT_SHALLOW TRUE
)

set(BUILD_SHELL OFF)
set(BUILD_TESTING OFF)
set(BUILD_UNITTESTS OFF)
set(EXTENSION_STATIC_BUILD ON)
# In-tree extensions besides the default core_functions and parquet:
# read_json() for JSON / NDJSON files.
set(BUILD_EXTENSIONS json)
# DuckDB takes its version from `git describe --tags` and falls back to v0.0.1
# when it fails, as it does in a shallow clone that has not fetched the tag.
set(OVERRIDE_GIT_DESCRIBE v${DUCKDB_VERSION})

FetchContent_MakeAvailable(duckdb)

# DuckDB forces the static MSVC runtime (/MT, /MTd) for its own directory,
# which conflicts with the dynamic runtime (/MD, /MDd) used by everything else.
# Switch every DuckDB target back to the dynamic runtime.
if(MSVC)
    function(tableskim_use_dynamic_msvc_runtime dir)
        get_property(targets DIRECTORY ${dir} PROPERTY BUILDSYSTEM_TARGETS)
        foreach(target IN LISTS targets)
            get_target_property(type ${target} TYPE)
            if(NOT type STREQUAL "INTERFACE_LIBRARY" AND NOT type STREQUAL "UTILITY")
                set_property(TARGET ${target} PROPERTY
                    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
            endif()
        endforeach()
        get_property(subdirs DIRECTORY ${dir} PROPERTY SUBDIRECTORIES)
        foreach(subdir IN LISTS subdirs)
            tableskim_use_dynamic_msvc_runtime(${subdir})
        endforeach()
    endfunction()
    tableskim_use_dynamic_msvc_runtime(${duckdb_SOURCE_DIR})
endif()

# duckdb_static does not link the built-in extensions by itself
# (only the shared "duckdb" target does), so bundle them here.
# duckdb_static also calls into the loader (LoadAllExtensions), so declare
# that dependency on duckdb_static itself; CMake then repeats the cyclic static
# libraries on the link line as single-pass linkers (GNU ld) require.
target_link_libraries(duckdb_static INTERFACE duckdb_generated_extension_loader)
target_link_libraries(duckdb_generated_extension_loader INTERFACE
    core_functions_extension
    parquet_extension
    json_extension
)
add_library(duckdb-static INTERFACE)
# duckdb_static references the extension loader, which references the
# extensions, which reference duckdb_static: a cycle, so rescan the group.
# Only GNU-style linkers need (and support) this; ld64 and link.exe already
# resolve symbols across archives regardless of order.
if(CMAKE_CXX_LINK_GROUP_USING_RESCAN_SUPPORTED)
    target_link_libraries(duckdb-static INTERFACE
        "$<LINK_GROUP:RESCAN,duckdb_generated_extension_loader,core_functions_extension,parquet_extension,json_extension,duckdb_static>"
    )
else()
    target_link_libraries(duckdb-static INTERFACE
        duckdb_generated_extension_loader
        core_functions_extension
        parquet_extension
        json_extension
        duckdb_static
    )
endif()
target_include_directories(duckdb-static SYSTEM INTERFACE
    ${duckdb_SOURCE_DIR}/src/include
)

# System SQLite is supported on Linux only.
if(UNIX AND NOT APPLE)
    option(TABLESKIM_USE_SYSTEM_SQLITE "Use the system SQLite instead of the bundled amalgamation" OFF)
else()
    set(TABLESKIM_USE_SYSTEM_SQLITE OFF)
endif()

if(TABLESKIM_USE_SYSTEM_SQLITE)
    find_package(SQLite3 REQUIRED)
    add_library(sqlite3 INTERFACE)
    target_link_libraries(sqlite3 INTERFACE SQLite::SQLite3)
else()
    FetchContent_Declare(
        sqlite
        SYSTEM
        URL https://sqlite.org/2026/sqlite-amalgamation-3540100.zip
        URL_HASH SHA256=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )

    FetchContent_MakeAvailable(sqlite)

    add_library(sqlite3
        STATIC
        ${sqlite_SOURCE_DIR}/sqlite3.c
        ${sqlite_SOURCE_DIR}/sqlite3.h
    )
    target_include_directories(sqlite3 SYSTEM PUBLIC ${sqlite_SOURCE_DIR})

    # sqlite3 lives in the top-level directory, so the override above does not
    # reach it; apply the Release flags to the target directly.
    if(NOT MSVC)
        target_compile_options(sqlite3 PRIVATE $<$<CONFIG:Debug>:-O3>)
        target_compile_definitions(sqlite3 PRIVATE $<$<CONFIG:Debug>:NDEBUG>)
    endif()
endif()

FetchContent_Declare(
    catch2
    SYSTEM
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.16.1
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(catch2)

# Catch2 adds a "d" postfix for Debug, but its directory is configured as
# Release above, so the Debug-named library would never be built.
set_target_properties(Catch2 Catch2WithMain PROPERTIES DEBUG_POSTFIX "")

FetchContent_Declare(
    ftxui
    SYSTEM
    GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
    GIT_TAG v7.0.3
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(ftxui)

# The monolithic (bundled) library relies on `ar -M`, which is unsupported by
# AppleClang/macOS ar and MSVC; link the regular OpenXLSX target instead.
set(OPENXLSX_MONOLITHIC_LIBRARY OFF)
set(BUILD_SHARED_LIBS OFF)
set(OPENXLSX_BUILD_SAMPLES OFF)
set(OPENXLSX_CREATE_DOCS OFF)
FetchContent_Declare(
    openxlsx
    SYSTEM
    GIT_REPOSITORY https://github.com/troldal/OpenXLSX.git
    GIT_TAG v0.5.1
    GIT_SHALLOW TRUE
)

# OpenXLSX fetches miniz 3.0.2, whose cmake_minimum_required(VERSION 3.0) is
# rejected by CMake 4.x. Allow it for this dependency only.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_MakeAvailable(openxlsx)
unset(CMAKE_POLICY_VERSION_MINIMUM)
# Fixes for reading numbers and inline strings (see the script). Run at every
# configure rather than as a PATCH_COMMAND, which an existing checkout would
# not run again when the script gains a fix; it leaves patched files alone.
execute_process(
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_LIST_DIR}/patches/openxlsx.cmake
    WORKING_DIRECTORY ${openxlsx_SOURCE_DIR}
    COMMAND_ERROR_IS_FATAL ANY
)

# FetchContent_Declare(
#     tiledb
#     GIT_REPOSITORY https://github.com/TileDB-Inc/TileDB.git
#     GIT_TAG 2.30.1
# )

# FetchContent_MakeAvailable(tiledb)

FetchContent_Declare(
  csv-parser
  SYSTEM
  GIT_REPOSITORY https://github.com/vincentlaucsb/csv-parser.git
  GIT_TAG v5.5.0
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(csv-parser)

FetchContent_Declare(
    CLI11
    SYSTEM
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG v2.7.2
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(CLI11)

# Decompression libraries for compressed CSV/TSV (.gz, .bz2, .xz, .zst).
# System copies are supported on Linux only.
if(UNIX AND NOT APPLE)
    option(TABLESKIM_USE_SYSTEM_COMPRESSION "Use the system zlib, bzip2, liblzma and zstd instead of building bundled copies" OFF)
else()
    set(TABLESKIM_USE_SYSTEM_COMPRESSION OFF)
endif()

if(TABLESKIM_USE_SYSTEM_COMPRESSION)
    find_package(ZLIB REQUIRED)
    find_package(BZip2 REQUIRED)
    find_package(LibLZMA REQUIRED)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)
    add_library(tableskim-compression INTERFACE)
    target_link_libraries(tableskim-compression INTERFACE
        ZLIB::ZLIB BZip2::BZip2 LibLZMA::LibLZMA PkgConfig::ZSTD)
else()
    set(ZLIB_BUILD_TESTING OFF)
    set(ZLIB_BUILD_SHARED OFF)
    set(ZLIB_INSTALL OFF)
    FetchContent_Declare(
        zlib
        SYSTEM
        URL https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz
        URL_HASH SHA256=bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(zlib)

    set(XZ_TOOL_XZ OFF)
    set(XZ_TOOL_XZDEC OFF)
    set(XZ_TOOL_LZMADEC OFF)
    set(XZ_TOOL_LZMAINFO OFF)
    set(XZ_NLS OFF)
    set(XZ_DOC OFF)
    FetchContent_Declare(
        xz
        SYSTEM
        URL https://github.com/tukaani-project/xz/releases/download/v5.8.4/xz-5.8.4.tar.gz
        URL_HASH SHA256=0014c7886930454fe8bd4228665b51af55eeae560ea135c9c4cd33f55b2591d9
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(xz)

    set(ZSTD_BUILD_PROGRAMS OFF)
    set(ZSTD_BUILD_TESTS OFF)
    set(ZSTD_BUILD_SHARED OFF)
    set(ZSTD_BUILD_STATIC ON)
    set(ZSTD_LEGACY_SUPPORT OFF)
    FetchContent_Declare(
        zstd
        SYSTEM
        URL https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
        URL_HASH SHA256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR build/cmake
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(zstd)

    # bzip2 1.0.8 has no CMake build; compile its sources directly.
    FetchContent_Declare(
        bzip2
        SYSTEM
        URL https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz
        URL_HASH SHA256=ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(bzip2)
    add_library(bz2 STATIC
        ${bzip2_SOURCE_DIR}/blocksort.c
        ${bzip2_SOURCE_DIR}/huffman.c
        ${bzip2_SOURCE_DIR}/crctable.c
        ${bzip2_SOURCE_DIR}/randtable.c
        ${bzip2_SOURCE_DIR}/compress.c
        ${bzip2_SOURCE_DIR}/decompress.c
        ${bzip2_SOURCE_DIR}/bzlib.c
    )
    target_include_directories(bz2 SYSTEM PUBLIC ${bzip2_SOURCE_DIR})
    # Without BZ_EXPORT, bzlib.h declares the API as function pointers on
    # Windows. Empty, as bzlib.h defines it again, so that it is not redefined.
    target_compile_definitions(bz2 PUBLIC BZ_EXPORT=)
    if(MSVC)
        target_compile_definitions(bz2 PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        # Like sqlite3, bz2 lives in the top-level directory.
        target_compile_options(bz2 PRIVATE $<$<CONFIG:Debug>:-O3>)
        target_compile_definitions(bz2 PRIVATE $<$<CONFIG:Debug>:NDEBUG>)
    endif()

    add_library(tableskim-compression INTERFACE)
    target_link_libraries(tableskim-compression INTERFACE
        zlibstatic bz2 liblzma libzstd_static)
    # liblzma adds its public headers to the include path only when installed.
    target_include_directories(tableskim-compression SYSTEM INTERFACE ${xz_SOURCE_DIR}/src/liblzma/api)
endif()

# Regular expression search. RE2 needs Abseil, which is added first so that
# RE2 uses these targets instead of looking for an installed Abseil.
set(ABSL_PROPAGATE_CXX_STD ON)
set(ABSL_BUILD_TESTING OFF)
set(ABSL_ENABLE_INSTALL OFF)
FetchContent_Declare(
    absl
    SYSTEM
    GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
    GIT_TAG 20260817.0
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(absl)

set(RE2_BUILD_TESTING OFF)
# Its install rules would export the Abseil targets, which are not installed.
set(RE2_INSTALL OFF)
FetchContent_Declare(
    re2
    SYSTEM
    GIT_REPOSITORY https://github.com/google/re2.git
    GIT_TAG 2025-11-05
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(re2)

FetchContent_Declare(
    spdlog
    SYSTEM
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.17.0
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(spdlog)

# Like Catch2, spdlog would otherwise be linked by its Debug name.
set_target_properties(spdlog PROPERTIES DEBUG_POSTFIX "")

# Restore the build type chosen for TableSkim itself.
unset(CMAKE_BUILD_TYPE)

if(WIN32)
    # Log file names are UTF-16 so that non-ASCII --log paths work.
    target_compile_definitions(spdlog PUBLIC SPDLOG_WCHAR_FILENAMES)
endif()