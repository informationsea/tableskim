# Collects the license texts of the bundled dependencies into
# THIRD_PARTY_LICENSES.txt and embeds it into the binary (see licenses.hxx).
#
# Every dependency is listed explicitly so that a new or renamed license file
# fails the configure step instead of being silently dropped. Dependencies
# that are not linked into the shipped binaries (Catch2) are omitted. One list
# serves both frontends, so it also names FLTK in the TUI and FTXUI in the GUI.
# Entries whose FetchContent source is absent (-DTABLESKIM_USE_SYSTEM_*=ON)
# are skipped: the system copy is not redistributed by TableSkim.
include(FetchContent)

set(TABLESKIM_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(TABLESKIM_LICENSE_FILE ${TABLESKIM_GENERATED_DIR}/THIRD_PARTY_LICENSES.txt)
file(WRITE ${TABLESKIM_LICENSE_FILE} "")

# tableskim_add_license(<label> [FETCH <FetchContent name>] [FILE <file>] [TEXT <text>])
# FILE is relative to the fetched source directory (or absolute without FETCH).
function(tableskim_add_license label)
    cmake_parse_arguments(ARG "" "FETCH;FILE;TEXT" "" ${ARGN})

    set(source_dir "")
    if(ARG_FETCH)
        FetchContent_GetProperties(${ARG_FETCH} SOURCE_DIR source_dir)
        if(NOT source_dir)
            message(STATUS "License of ${label} skipped (not bundled)")
            return()
        endif()
    endif()

    set(body "${ARG_TEXT}")
    if(ARG_FILE)
        set(path "${ARG_FILE}")
        if(source_dir)
            set(path "${source_dir}/${ARG_FILE}")
        endif()
        if(NOT EXISTS "${path}")
            message(FATAL_ERROR "License file of ${label} not found: ${path}")
        endif()
        # Re-run CMake when a license file changes (e.g. a dependency update).
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
        file(READ "${path}" body)
        string(REPLACE "\r\n" "\n" body "${body}")
    endif()

    set(rule "================================================================================")
    file(APPEND ${TABLESKIM_LICENSE_FILE} "${rule}\n${label}\n${rule}\n\n${body}\n\n")
endfunction()

tableskim_add_license("TableSkim" FILE ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE)
tableskim_add_license("FLTK" FETCH fltk FILE COPYING)
# Libraries bundled inside FLTK (FLTK_USE_SYSTEM_* are OFF).
tableskim_add_license("FLTK bundled: libpng" FETCH fltk FILE png/LICENSE)
tableskim_add_license("FLTK bundled: libjpeg" FETCH fltk FILE jpeg/README)
tableskim_add_license("FLTK bundled: zlib" FETCH fltk FILE zlib/README)
# nanosvg is header-only; its license is the comment at the top of nanosvg.h.
tableskim_add_license("FLTK bundled: nanosvg" FETCH fltk TEXT
"Copyright (c) 2013-14 Mikko Mononen memon@inside.org

This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
claim that you wrote the original software. If you use this software
in a product, an acknowledgment in the product documentation would be
appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.

The SVG parser is based on Anti-Grain Geometry 2.4 SVG example
Copyright (C) 2002-2004 Maxim Shemanarev (McSeem) (http://www.antigrain.com/)")
if(UNIX AND NOT APPLE)
    tableskim_add_license("FLTK bundled: libdecor" FETCH fltk FILE libdecor/LICENSE)
endif()
# Fonts embedded into the GUI (gui.fl).
if(TABLESKIM_BUILD_GUI)
    tableskim_add_license("Font: BIZ UDPGothic" FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/fonts/OFL-BIZUDGothic.txt)
    tableskim_add_license("Font: M PLUS 1 Code" FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/fonts/OFL-MPLUS.txt)
endif()

tableskim_add_license("DuckDB" FETCH duckdb FILE LICENSE)
# DuckDB's own third_party/ directory (test-only catch and imdb are omitted).
foreach(name IN ITEMS
        brotli concurrentqueue fast_float fastpforlib fmt fsst httplib
        hyperloglog jaro_winkler jemalloc libpg_query lz4 mbedtls miniz
        parquet pcg pdqsort re2 ska_sort skiplist snappy snowball tdigest
        utf8proc vergesort yyjson zstd)
    tableskim_add_license("DuckDB bundled: ${name}" FETCH duckdb FILE third_party/${name}/LICENSE)
endforeach()
tableskim_add_license("DuckDB bundled: tdigest (notices)" FETCH duckdb FILE third_party/tdigest/NOTICES)
tableskim_add_license("DuckDB bundled: thrift" FETCH duckdb FILE third_party/thrift/thrift/LICENSE)
tableskim_add_license("SQLite" FETCH sqlite
    TEXT "The author disclaims copyright to the SQLite source code. It is in the public domain.")
tableskim_add_license("OpenXLSX" FETCH openxlsx FILE LICENSE.md)
tableskim_add_license("miniz" FETCH miniz_fetch FILE LICENSE)
tableskim_add_license("pugixml" FETCH pugixml_fetch FILE LICENSE.md)
tableskim_add_license("Boost.Nowide" FETCH nowide_fetch FILE LICENSE)
tableskim_add_license("csv-parser" FETCH csv-parser FILE LICENSE)
tableskim_add_license("FTXUI" FETCH ftxui FILE LICENSE)
tableskim_add_license("CLI11" FETCH cli11 FILE LICENSE)
tableskim_add_license("spdlog" FETCH spdlog FILE LICENSE)
tableskim_add_license("zlib" FETCH zlib FILE LICENSE)
tableskim_add_license("bzip2" FETCH bzip2 FILE LICENSE)
tableskim_add_license("liblzma (XZ Utils)" FETCH xz FILE COPYING.0BSD)
tableskim_add_license("Zstandard" FETCH zstd FILE LICENSE)
tableskim_add_license("RE2" FETCH re2 FILE LICENSE)
tableskim_add_license("Abseil" FETCH absl FILE LICENSE)

# Embed the text as a byte array: MSVC rejects string literals over ~16 KB.
file(READ ${TABLESKIM_LICENSE_FILE} license_hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," license_bytes "${license_hex}")
# CMake regular expressions have no {n} repetition; wrap every 8 bytes.
string(REPEAT "(0x[0-9a-f][0-9a-f],)" 8 license_line)
string(REGEX REPLACE "(${license_line})" "\\1\n" license_bytes "${license_bytes}")
file(CONFIGURE OUTPUT ${TABLESKIM_GENERATED_DIR}/third_party_licenses.cxx
    CONTENT "// Generated by cmake/Licenses.cmake; do not edit.
#include \"licenses.hxx\"

namespace
{
    // NUL-terminated so that data() of the view is a valid C string.
    const unsigned char license_data[] = {
@license_bytes@
        0x00};
} // namespace

std::string_view thirdPartyLicenses()
{
    return std::string_view(reinterpret_cast<const char *>(license_data), sizeof(license_data) - 1);
}
"
    @ONLY)
