cmake_minimum_required(VERSION 3.26)

# macOS builds (Apple silicon and Intel alike) run on macOS 12 and later,
# not only on the version they were built on. Set before project() so that
# the dependencies get it too; a directory configured without it has it
# cached empty, hence FORCE. MACOSX_DEPLOYMENT_TARGET or
# -DCMAKE_OSX_DEPLOYMENT_TARGET=... overrides it.
if(CMAKE_HOST_APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET AND NOT DEFINED ENV{MACOSX_DEPLOYMENT_TARGET})
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version to run on" FORCE)
endif()

project(table-skim VERSION 0.3.3 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MSVC reports __cplusplus as 199711L unless /Zc:__cplusplus is given,
# which makes dependencies (e.g. DuckDB) take pre-C++17 code paths.
# /MP3 compiles up to 3 files of a project in parallel; combined with
# "jobs": 2 in CMakePresets.json this caps the build at 6 concurrent cl.exe.
# Ninja runs one cl.exe per file by itself, so /MP is left out there.
if(MSVC)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>)
    if(NOT CMAKE_GENERATOR MATCHES "Ninja")
        add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/MP3>)
    endif()
endif()

# WebAssembly (Emscripten): the core library and its tests only, for a
# browser frontend. Every object must agree on threads and on the exception
# model, so set both before the dependencies are added.
if(EMSCRIPTEN)
    set(TABLESKIM_BUILD_GUI OFF)
    add_compile_options(-pthread -fwasm-exceptions)
    add_link_options(-pthread -fwasm-exceptions)
endif()

option(TABLESKIM_BUILD_GUI "Build the FLTK GUI; when OFF, tableskim is the terminal UI only and FLTK is not needed" ON)

# Reuse compiled objects through ccache when it is installed, for this project
# and every dependency (set before they are added). Visual Studio generators
# ignore compiler launchers, so this applies to Ninja and Makefiles only.
option(TABLESKIM_USE_CCACHE "Compile through ccache when it is found" ON)
if(TABLESKIM_USE_CCACHE AND CMAKE_GENERATOR MATCHES "Ninja|Makefiles")
    find_program(TABLESKIM_CCACHE ccache)
    if(TABLESKIM_CCACHE)
        message(STATUS "Using ccache: ${TABLESKIM_CCACHE}")
        set(CMAKE_C_COMPILER_LAUNCHER ${TABLESKIM_CCACHE})
        set(CMAKE_CXX_COMPILER_LAUNCHER ${TABLESKIM_CCACHE})
    endif()
endif()

# CMake 3.28+ adds -fmodules-ts to C++20 sources of dependencies that ask for
# the new policies (DuckDB), which ccache does not support, so those files were
# never cached. Nothing here uses C++ modules.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

include(cmake/Dependencies.cmake)
include(cmake/Licenses.cmake)

# Warnings for TableSkim's own targets only: the dependencies keep their own
# flags, and their headers are system headers (SYSTEM in
# cmake/Dependencies.cmake), so their warnings stay out.
add_library(tableskim-warnings INTERFACE)
if(MSVC)
    target_compile_options(tableskim-warnings INTERFACE /W4)
else()
    # Designated initializers leave the members they skip at their defaults
    # on purpose, which GCC reports as missing.
    target_compile_options(tableskim-warnings INTERFACE -Wall -Wextra -Wno-missing-field-initializers)
endif()

add_library(tableskim-common
    STATIC
    src/common/common.hxx
    src/common/common.cxx
    src/common/largefile.hxx
    src/common/largefile.cxx
    src/common/table_types.hxx
    src/common/table_source.hxx
    src/common/table_registry.cxx
    src/common/table.hxx
    src/common/table.cxx
    src/common/column_stats.hxx
    src/common/column_stats.cxx
    src/common/histogram.hxx
    src/common/histogram.cxx
    src/common/scatter.hxx
    src/common/scatter.cxx
    src/common/table_csv.hxx
    src/common/table_csv.cxx
    src/common/table_excel.hxx
    src/common/table_excel.cxx
    src/common/table_duckdb.hxx
    src/common/table_duckdb.cxx
    src/common/table_parquet.hxx
    src/common/table_parquet.cxx
    src/common/table_json.hxx
    src/common/table_json.cxx
    src/common/table_export.hxx
    src/common/sql_text.hxx
    src/common/sql_text.cxx
    src/common/search_hits.hxx
    src/common/search_hits.cxx
    src/common/sql_history.hxx
    src/common/sql_history.cxx
    src/common/recent_files.hxx
    src/common/recent_files.cxx
    src/common/column_type.hxx
    src/common/column_type.cxx
    src/common/group_by.hxx
    src/common/group_by.cxx
    src/common/markdown.hxx
    src/common/markdown.cxx
    src/common/help_docs.hxx
    src/common/help_docs.cxx
    src/common/table_export.cxx
    src/common/table_summary.hxx
    src/common/table_summary.cxx
    src/common/xlsx_writer.hxx
    src/common/xlsx_writer.cxx
    src/common/table_sqlite.hxx
    src/common/table_sqlite.cxx
    src/common/decompress.hxx
    src/common/decompress.cxx
    src/common/compress.hxx
    src/common/compress.cxx
    src/common/text_encoding.hxx
    src/common/text_encoding.cxx
    src/common/cp932_table.hxx
    src/common/cp932_table.cxx
    src/common/licenses.hxx
    src/common/cell_format.hxx
    src/common/cell_format.cxx
    src/common/file_util.hxx
    src/common/file_util.cxx
    src/common/cli_options.hxx
    src/common/cli_options.cxx
    ${CMAKE_CURRENT_BINARY_DIR}/generated/third_party_licenses.cxx
)

configure_file(src/common/tableskim_version.hxx.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/tableskim_version.hxx @ONLY)

# The git revision is looked up on every build rather than at configure time,
# so that it stays current in a reused build directory.
find_package(Git QUIET)
set(TABLESKIM_GIT_REVISION_HXX ${CMAKE_CURRENT_BINARY_DIR}/generated/tableskim_git_revision.hxx)
add_custom_target(tableskim-git-revision ALL
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DOUTPUT=${TABLESKIM_GIT_REVISION_HXX}
        -DGIT_EXECUTABLE=${GIT_EXECUTABLE}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/GitRevision.cmake
    BYPRODUCTS ${TABLESKIM_GIT_REVISION_HXX}
    COMMENT "Checking git revision"
    VERBATIM
)
add_dependencies(tableskim-common tableskim-git-revision)

target_include_directories(tableskim-common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/common
    ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(tableskim-common PUBLIC duckdb-static sqlite3 spdlog::spdlog csv OpenXLSX::OpenXLSX)
target_link_libraries(tableskim-common PRIVATE pugixml::pugixml tableskim-compression CLI11::CLI11 re2::re2 absl::strings tableskim-warnings)
target_compile_definitions(tableskim-common PRIVATE
    $<$<CONFIG:Debug>:TABLESKIM_DEBUG=1>
)
if(WIN32)
    # openWithFileBrowser()
    target_link_libraries(tableskim-common PRIVATE shell32 ole32)
endif()

# Opens a file, reads it through and sorts it, timing each step, in Node.js
# or a browser (src/wasm/README.md); natively too, to compare. Not built by
# default: nothing runs it, and its link is slow in WebAssembly.
add_executable(tableskim-wasm-probe src/wasm/wasm_probe.cxx)
target_link_libraries(tableskim-wasm-probe PRIVATE tableskim-common tableskim-warnings)
set_target_properties(tableskim-wasm-probe PROPERTIES EXCLUDE_FROM_ALL ON)

if(EMSCRIPTEN)
    # main() gets a thread of its own, so that the thread running the
    # runtime is free to start the workers that main() waits for (and to
    # serve the file systems, which only it may touch). DuckDB needs more
    # stack than the default 64 KiB.
    set(TABLESKIM_WASM_LINK_OPTIONS
        -sPROXY_TO_PTHREAD=1
        -sEXIT_RUNTIME=1
        -sALLOW_MEMORY_GROWTH=1
        -sMAXIMUM_MEMORY=4GB
        -sSTACK_SIZE=8MB
        -sDEFAULT_PTHREAD_STACK_SIZE=8MB
    )

    # Node.js and browsers (as a worker's script) read the files it opens
    # through the file systems wasm_probe_pre.js mounts.
    target_link_options(tableskim-wasm-probe PRIVATE
        ${TABLESKIM_WASM_LINK_OPTIONS}
        -sENVIRONMENT=web,worker,node
        -lnodefs.js
        -lworkerfs.js
        --pre-js ${CMAKE_CURRENT_SOURCE_DIR}/src/wasm/wasm_probe_pre.js
    )
    set_property(TARGET tableskim-wasm-probe APPEND PROPERTY LINK_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/src/wasm/wasm_probe_pre.js)

    # The module of the web frontend (web/app/), run as a worker's script.
    add_executable(tableskim-wasm
        src/wasm/wasm_api.cxx
        src/wasm/web_session.hxx
        src/wasm/web_session.cxx
        src/wasm/web_json.hxx
        src/wasm/web_json.cxx
    )
    target_link_libraries(tableskim-wasm PRIVATE tableskim-common tableskim-warnings)
    target_link_options(tableskim-wasm PRIVATE
        ${TABLESKIM_WASM_LINK_OPTIONS}
        -sEXIT_RUNTIME=0
        -sENVIRONMENT=worker
        -sFORCE_FILESYSTEM=1
        -sEXPORTED_RUNTIME_METHODS=stringToNewUTF8,UTF8ToString
        -lworkerfs.js
        --pre-js ${CMAKE_CURRENT_SOURCE_DIR}/src/wasm/wasm_worker_pre.js
    )
    set_property(TARGET tableskim-wasm APPEND PROPERTY LINK_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/src/wasm/wasm_worker_pre.js)

    # The same module for Node.js, run in a worker_threads Worker by the VS
    # Code extension (vscode/), which opens files on the host with NODEFS.
    # Not built by default: the extension's build asks for it.
    add_executable(tableskim-wasm-node
        src/wasm/wasm_api.cxx
        src/wasm/web_session.hxx
        src/wasm/web_session.cxx
        src/wasm/web_json.hxx
        src/wasm/web_json.cxx
    )
    target_link_libraries(tableskim-wasm-node PRIVATE tableskim-common tableskim-warnings)
    set_target_properties(tableskim-wasm-node PROPERTIES EXCLUDE_FROM_ALL ON)
    target_link_options(tableskim-wasm-node PRIVATE
        ${TABLESKIM_WASM_LINK_OPTIONS}
        -sEXIT_RUNTIME=0
        -sENVIRONMENT=node
        -sFORCE_FILESYSTEM=1
        -sEXPORTED_RUNTIME_METHODS=stringToNewUTF8,UTF8ToString
        -lnodefs.js
        --pre-js ${CMAKE_CURRENT_SOURCE_DIR}/src/wasm/wasm_worker_pre.js
    )
    set_property(TARGET tableskim-wasm-node APPEND PROPERTY LINK_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/src/wasm/wasm_worker_pre.js)

    # The web app itself (web/: React, TypeScript, Vite), built with npm into
    # <build>/web with the module next to it. AUTO builds it when npm is
    # found, ON requires it; npm ci fetches its packages the first time.
    set(TABLESKIM_WEB_APP AUTO CACHE STRING "Build the web app with npm (AUTO, ON or OFF; WebAssembly builds only)")
    set_property(CACHE TABLESKIM_WEB_APP PROPERTY STRINGS AUTO ON OFF)
    set(TABLESKIM_HAVE_WEB_APP FALSE)
    if(NOT TABLESKIM_WEB_APP STREQUAL "OFF")
        find_program(TABLESKIM_NPM npm)
        if(TABLESKIM_NPM)
            set(TABLESKIM_HAVE_WEB_APP TRUE)
        elseif(TABLESKIM_WEB_APP STREQUAL "ON")
            message(FATAL_ERROR "TABLESKIM_WEB_APP=ON needs npm (Node.js)")
        else()
            message(STATUS "Web app skipped: npm not found")
        endif()
    endif()
    if(TABLESKIM_HAVE_WEB_APP)
        set(TABLESKIM_WEB_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/web)
        set(TABLESKIM_WEB_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/web)
        add_custom_command(
            OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/web-packages.stamp
            COMMAND ${TABLESKIM_NPM} ci --no-audit --no-fund
            COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_CURRENT_BINARY_DIR}/web-packages.stamp
            DEPENDS ${TABLESKIM_WEB_SOURCE_DIR}/package.json ${TABLESKIM_WEB_SOURCE_DIR}/package-lock.json
            WORKING_DIRECTORY ${TABLESKIM_WEB_SOURCE_DIR}
            COMMENT "Installing the web app's packages"
            VERBATIM
        )
        file(GLOB_RECURSE TABLESKIM_WEB_SOURCES CONFIGURE_DEPENDS
            ${TABLESKIM_WEB_SOURCE_DIR}/src/*
            ${TABLESKIM_WEB_SOURCE_DIR}/public/*
        )
        # A removed input must rebuild too, so Vite clears its old output.
        file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/web-inputs.txt
            CONTENT "@TABLESKIM_WEB_SOURCES@\n" @ONLY)
        add_custom_command(
            OUTPUT ${TABLESKIM_WEB_OUTPUT_DIR}/index.html
            COMMAND ${CMAKE_COMMAND} -E env TABLESKIM_WASM_BUILD=${CMAKE_CURRENT_BINARY_DIR}
                ${TABLESKIM_NPM} run build -- --outDir ${TABLESKIM_WEB_OUTPUT_DIR} --emptyOutDir
            DEPENDS
                ${CMAKE_CURRENT_BINARY_DIR}/web-packages.stamp
                ${TABLESKIM_WEB_SOURCES}
                ${CMAKE_CURRENT_BINARY_DIR}/web-inputs.txt
                ${TABLESKIM_WEB_SOURCE_DIR}/index.html
                ${TABLESKIM_WEB_SOURCE_DIR}/vite.config.ts
                ${TABLESKIM_WEB_SOURCE_DIR}/tsconfig.json
                tableskim-wasm
            WORKING_DIRECTORY ${TABLESKIM_WEB_SOURCE_DIR}
            COMMENT "Building the web app"
            VERBATIM
        )
        add_custom_target(tableskim-web ALL DEPENDS ${TABLESKIM_WEB_OUTPUT_DIR}/index.html)
        # The app as a ZIP to put on any web server, with the licenses and a
        # README (the release's tableskim-<version>-web.zip); built on request.
        set(TABLESKIM_WEB_ZIP_NAME tableskim-${PROJECT_VERSION}-web)
        add_custom_target(tableskim-web-zip
            COMMAND ${CMAKE_COMMAND}
                -DAPP_DIR=${TABLESKIM_WEB_OUTPUT_DIR}
                -DSTAGE_DIR=${CMAKE_CURRENT_BINARY_DIR}/web-zip/${TABLESKIM_WEB_ZIP_NAME}
                -DZIP_FILE=${CMAKE_CURRENT_BINARY_DIR}/${TABLESKIM_WEB_ZIP_NAME}.zip
                -DLICENSE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
                -DTHIRD_PARTY_FILE=${TABLESKIM_LICENSE_FILE}
                -DWEB_SOURCE_DIR=${TABLESKIM_WEB_SOURCE_DIR}
                -DEMSCRIPTEN_LICENSE=${EMSCRIPTEN_ROOT_PATH}/LICENSE
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/WebZip.cmake
            DEPENDS tableskim-web
            COMMENT "Packing the web app into ${TABLESKIM_WEB_ZIP_NAME}.zip"
            VERBATIM
        )
    endif()
endif()

set(TABLESKIM_TUI_SOURCES
    src/main.cxx
    src/tui/tui_main.hxx
    src/tui/tui_main.cxx
    src/tui/tui_app.hxx
    src/tui/tui_app.cxx
    src/tui/tui_table_view.hxx
    src/tui/tui_table_view.cxx
    src/tui/tui_text.hxx
    src/tui/tui_text.cxx
)

# The single `tableskim` command: it starts the GUI or, without a display
# (or with --tui), the terminal UI. A WebAssembly build has no command.
if(NOT EMSCRIPTEN)
    add_executable(tableskim ${TABLESKIM_TUI_SOURCES})
    target_include_directories(tableskim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/tui)
    target_link_libraries(tableskim PRIVATE tableskim-common ftxui::screen ftxui::dom ftxui::component tableskim-warnings)
    target_compile_definitions(tableskim PRIVATE
        $<$<CONFIG:Debug>:TABLESKIM_DEBUG=1>
    )
    add_dependencies(tableskim tableskim-git-revision)
endif()

# On Windows the GUI executable has no console in Release builds, so the
# terminal UI gets a console-subsystem executable of its own. Without
# TABLESKIM_HAS_GUI, main() always runs the TUI.
if(WIN32 AND TABLESKIM_BUILD_GUI)
    add_executable(tableskim-tui ${TABLESKIM_TUI_SOURCES})
    target_include_directories(tableskim-tui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/tui)
    target_link_libraries(tableskim-tui PRIVATE tableskim-common ftxui::screen ftxui::dom ftxui::component tableskim-warnings)
    target_compile_definitions(tableskim-tui PRIVATE
        $<$<CONFIG:Debug>:TABLESKIM_DEBUG=1>
    )
    add_dependencies(tableskim-tui tableskim-git-revision)
endif()

if(TABLESKIM_BUILD_GUI)
    # The GUI is a library of its own, so that tableskim-gui-tests can drive
    # it too (tests/gui/).
    add_library(tableskim-gui STATIC
        src/gui/gui_main.hxx
        src/gui/gui_main.cxx
        src/gui/gui_callbacks.hxx
        src/gui/gui_callbacks.cxx
        src/gui/gui_impl.cxx
        src/gui/gui_convert.hxx
        src/gui/gui_convert.cxx
        src/gui/gui_sql.hxx
        src/gui/gui_sql.cxx
        src/gui/gui_table_view.hxx
        src/gui/gui_table_view.cxx
        src/gui/gui_search_results.hxx
        src/gui/gui_search_results.cxx
        src/gui/gui_filter_pane.hxx
        src/gui/gui_filter_pane.cxx
        src/gui/gui_stats_pane.hxx
        src/gui/gui_stats_pane.cxx
        src/gui/gui_histogram.hxx
        src/gui/gui_histogram.cxx
        src/gui/gui_scatter.hxx
        src/gui/gui_scatter.cxx
        src/gui/gui_group_by.hxx
        src/gui/gui_group_by.cxx
        src/gui/gui_image_export.hxx
        src/gui/gui_image_export.cxx
        src/gui/gui_help.hxx
        src/gui/gui_help.cxx
        src/gui/gui_fonts.hxx
        src/gui/gui_fonts.cxx
        src/gui/gui_theme.hxx
        src/gui/gui_theme.cxx
        src/gui/gui_window_menu.hxx
        src/gui/gui_window_menu.cxx
    )
    fltk_wrap_ui(tableskim-gui
        src/gui/gui.fl
    )
    # The user manual, built into the program for Help > User Manual.
    file(GLOB TABLESKIM_HELP_FILES CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/docs
        ${CMAKE_CURRENT_SOURCE_DIR}/docs/*/*.md
        ${CMAKE_CURRENT_SOURCE_DIR}/docs/images/*.png
    )
    list(SORT TABLESKIM_HELP_FILES)
    list(TRANSFORM TABLESKIM_HELP_FILES PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/docs/ OUTPUT_VARIABLE TABLESKIM_HELP_PATHS)
    string(REPLACE ";" "|" TABLESKIM_HELP_LIST "${TABLESKIM_HELP_FILES}")
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/help_files.cxx
        COMMAND ${CMAKE_COMMAND}
            -DBASE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/docs
            -DFILES=${TABLESKIM_HELP_LIST}
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/help_files.cxx
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFiles.cmake
        DEPENDS ${TABLESKIM_HELP_PATHS} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFiles.cmake
        COMMENT "Embedding the user manual"
        VERBATIM
    )
    target_sources(tableskim-gui PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/help_files.cxx)
    target_sources(tableskim-gui PRIVATE ${tableskim-gui_FLTK_UI_SRCS})
    target_include_directories(tableskim-gui PUBLIC
        ${CMAKE_CURRENT_BINARY_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src/gui
    )
    target_link_libraries(tableskim-gui PUBLIC tableskim-common fltk::fltk fltk::images)
    target_link_libraries(tableskim-gui PRIVATE tableskim-warnings)
    target_compile_definitions(tableskim-gui PRIVATE
        $<$<CONFIG:Debug>:TABLESKIM_DEBUG=1>
    )
    if(UNIX AND NOT APPLE)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
        target_link_libraries(tableskim-gui PUBLIC PkgConfig::FONTCONFIG)
    endif()
    if(WIN32)
        target_link_libraries(tableskim-gui PUBLIC shell32 ole32)
    endif()

    target_link_libraries(tableskim PRIVATE tableskim-gui)
    target_compile_definitions(tableskim PRIVATE TABLESKIM_HAS_GUI=1)
    set_target_properties(tableskim PROPERTIES MACOSX_BUNDLE ON)
endif()

if(APPLE AND TABLESKIM_BUILD_GUI)
    set_target_properties(tableskim PROPERTIES
        OUTPUT_NAME "TableSkim"
        MACOSX_BUNDLE_BUNDLE_NAME "TableSkim"
        MACOSX_BUNDLE_GUI_IDENTIFIER "info.informationsea.TableSkim"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "1"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/Info.plist.in"
    )


    # Use pixel-fitted artwork for the smallest ICNS representations.
    set(TABLESKIM_ICON_PNG ${CMAKE_CURRENT_SOURCE_DIR}/src/images/icon.png)
    set(TABLESKIM_ICONSET ${CMAKE_CURRENT_BINARY_DIR}/TableSkim.iconset)
    set(TABLESKIM_ICNS ${CMAKE_CURRENT_BINARY_DIR}/TableSkim.icns)

    set(TABLESKIM_ICON_COMMANDS)
    foreach(size 16 32 128 256 512)
        math(EXPR size2x "${size} * 2")
        set(icon_source ${TABLESKIM_ICON_PNG})
        set(icon_source2x ${TABLESKIM_ICON_PNG})
        if(size EQUAL 16)
            set(icon_source ${CMAKE_CURRENT_SOURCE_DIR}/src/images/icon-16.png)
            set(icon_source2x ${CMAKE_CURRENT_SOURCE_DIR}/src/images/icon-32.png)
        elseif(size EQUAL 32)
            set(icon_source ${CMAKE_CURRENT_SOURCE_DIR}/src/images/icon-32.png)
            set(icon_source2x ${CMAKE_CURRENT_SOURCE_DIR}/src/images/icon-64.png)
        endif()
        list(APPEND TABLESKIM_ICON_COMMANDS
            COMMAND sips -z ${size} ${size} ${icon_source}
                --out ${TABLESKIM_ICONSET}/icon_${size}x${size}.png
            COMMAND sips -z ${size2x} ${size2x} ${icon_source2x}
                --out ${TABLESKIM_ICONSET}/icon_${size}x${size}@2x.png
        )
    endforeach()

    add_custom_command(
        OUTPUT ${TABLESKIM_ICNS}
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${TABLESKIM_ICONSET}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${TABLESKIM_ICONSET}
        ${TABLESKIM_ICON_COMMANDS}
        COMMAND iconutil -c icns ${TABLESKIM_ICONSET} -o ${TABLESKIM_ICNS}
        DEPENDS ${TABLESKIM_ICON_PNG}
            ${CMAKE_CURRENT_SOURCE_DIR}/src/images/icon-16.png
            ${CMAKE_CURRENT_SOURCE_DIR}/src/images/icon-32.png
            ${CMAKE_CURRENT_SOURCE_DIR}/src/images/icon-64.png
        COMMENT "Generating TableSkim.icns"
        VERBATIM
    )

    target_sources(tableskim PRIVATE ${TABLESKIM_ICNS})
    set_source_files_properties(${TABLESKIM_ICNS} PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources
    )
    set_target_properties(tableskim PROPERTIES
        MACOSX_BUNDLE_ICON_FILE "TableSkim.icns"
    )

    # Document icons named by CFBundleTypeIconFile in Info.plist.in,
    # generated by src/images/filetypes/make-icons.py
    set(TABLESKIM_FILETYPE_ICNS)
    foreach(format csv tsv xlsx xlsm parquet duckdb sqlite json vcf)
        list(APPEND TABLESKIM_FILETYPE_ICNS ${CMAKE_CURRENT_SOURCE_DIR}/src/images/filetypes/${format}.icns)
    endforeach()
    target_sources(tableskim PRIVATE ${TABLESKIM_FILETYPE_ICNS})
    set_source_files_properties(${TABLESKIM_FILETYPE_ICNS} PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources
    )
endif()

if(WIN32)
    # icon.ico includes pixel-fitted artwork for sizes from 16x16 through 64x64.
    set(TABLESKIM_ICON_ICO ${CMAKE_CURRENT_SOURCE_DIR}/src/images/icon.ico)
    # Generated by src/images/filetypes/make-icons.py
    set(TABLESKIM_FILETYPE_ICON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/images/filetypes)
    configure_file(src/gui/tableskim.rc.in ${CMAKE_CURRENT_BINARY_DIR}/tableskim.rc @ONLY)
    target_sources(tableskim PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/tableskim.rc)
    if(TARGET tableskim-tui)
        target_sources(tableskim-tui PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/tableskim.rc)
    endif()
    set_target_properties(tableskim PROPERTIES OUTPUT_NAME "TableSkim")
    if(TABLESKIM_BUILD_GUI)
        # Hide the console window only in Release builds (the TUI allocates
        # a console of its own when it has none)
        set_target_properties(tableskim PROPERTIES WIN32_EXECUTABLE $<CONFIG:Release>)
    endif()
endif()

if(UNIX AND NOT APPLE AND TABLESKIM_BUILD_GUI)
    # Generate hicolor theme icons, using pixel-fitted small artwork where available.
    add_executable(tableskim-icon-resize src/tools/icon_resize.cxx)
    target_link_libraries(tableskim-icon-resize PRIVATE fltk::images tableskim-warnings)

    set(TABLESKIM_ICON_PNG ${CMAKE_CURRENT_SOURCE_DIR}/src/images/icon.png)
    set(TABLESKIM_ICON_SIZES 16 24 32 48 64 128 256 512)
    set(TABLESKIM_ICON_OUTPUTS)
    foreach(size ${TABLESKIM_ICON_SIZES})
        set(icon ${CMAKE_CURRENT_BINARY_DIR}/icons/${size}x${size}/tableskim.png)
        set(icon_source ${TABLESKIM_ICON_PNG})
        if(size EQUAL 16)
            set(icon_source ${CMAKE_CURRENT_SOURCE_DIR}/src/images/icon-16.png)
        elseif(size EQUAL 32)
            set(icon_source ${CMAKE_CURRENT_SOURCE_DIR}/src/images/icon-32.png)
        elseif(size EQUAL 48)
            set(icon_source ${CMAKE_CURRENT_SOURCE_DIR}/src/images/icon-48.png)
        elseif(size EQUAL 64)
            set(icon_source ${CMAKE_CURRENT_SOURCE_DIR}/src/images/icon-64.png)
        endif()
        add_custom_command(
            OUTPUT ${icon}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/icons/${size}x${size}
            COMMAND tableskim-icon-resize ${icon_source} ${icon} ${size}
            DEPENDS ${icon_source} tableskim-icon-resize
            COMMENT "Generating ${size}x${size} icon"
            VERBATIM
        )
        list(APPEND TABLESKIM_ICON_OUTPUTS ${icon})
    endforeach()
    add_custom_target(tableskim-icons ALL DEPENDS ${TABLESKIM_ICON_OUTPUTS})
endif()

# The user manual as HTML for browsers, installed with the program (see
# cmake/Packaging.cmake). The tool runs at build time, so a WebAssembly build
# leaves it out.
if(NOT EMSCRIPTEN)
    add_executable(tableskim-html-manual
        src/tools/html_manual.cxx
        src/common/markdown.cxx
        src/common/help_docs.cxx
    )
    target_include_directories(tableskim-html-manual PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/common)
    target_link_libraries(tableskim-html-manual PRIVATE tableskim-warnings)
    file(GLOB_RECURSE TABLESKIM_MANUAL_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/docs/*.md
        ${CMAKE_CURRENT_SOURCE_DIR}/docs/*.png
    )
    set(TABLESKIM_MANUAL_DIR ${CMAKE_CURRENT_BINARY_DIR}/manual)
    add_custom_command(
        OUTPUT ${TABLESKIM_MANUAL_DIR}/README.html
        COMMAND tableskim-html-manual ${CMAKE_CURRENT_SOURCE_DIR}/docs ${TABLESKIM_MANUAL_DIR}
        DEPENDS ${TABLESKIM_MANUAL_SOURCES} tableskim-html-manual
        COMMENT "Converting the user manual to HTML"
        VERBATIM
    )
    add_custom_target(tableskim-manual ALL DEPENDS ${TABLESKIM_MANUAL_DIR}/README.html)
endif()

enable_testing()

add_executable(tableskim-tests
    tests/test_main.cxx
    tests/table_tests.cxx
    tests/table_csv_tests.cxx
    tests/table_excel_tests.cxx
    tests/table_duckdb_tests.cxx
    tests/table_parquet_tests.cxx
    tests/table_json_tests.cxx
    tests/table_export_tests.cxx
    tests/sql_tests.cxx
    tests/recent_files_tests.cxx
    tests/group_by_tests.cxx
    tests/column_type_tests.cxx
    tests/follow_tests.cxx
    tests/table_sqlite_tests.cxx
    tests/decompress_tests.cxx
    tests/compress_tests.cxx
    tests/editing_tests.cxx
    tests/text_encoding_tests.cxx
    tests/column_stats_tests.cxx
    tests/histogram_tests.cxx
    tests/scatter_tests.cxx
    tests/markdown_tests.cxx
    tests/licenses_tests.cxx
    tests/cell_format_tests.cxx
    tests/tui_text_tests.cxx
    tests/search_hits_tests.cxx
    tests/cli_options_tests.cxx
    tests/table_summary_tests.cxx
    tests/web_session_tests.cxx
    src/tui/tui_text.cxx
    src/wasm/web_session.cxx
    src/wasm/web_json.cxx
)
target_include_directories(tableskim-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/tui ${CMAKE_CURRENT_SOURCE_DIR}/src/wasm)
target_link_libraries(tableskim-tests PRIVATE tableskim-common Catch2::Catch2 pugixml::pugixml ftxui::screen tableskim-warnings)
target_compile_definitions(tableskim-tests PRIVATE
    TABLESKIM_DEMO_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/demo/data"
    TABLESKIM_DOCS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/docs"
    $<$<BOOL:${TABLESKIM_BUILD_GUI}>:TABLESKIM_TESTS_HAVE_GUI=1>
)
if(EMSCRIPTEN)
    # Run by Node.js (ctest goes through the toolchain's emulator), reading
    # the host files directly. One ctest test for them all: each process
    # loads the whole module again, which per test case would take minutes.
    # -O1 after the build type's -O3 skips Binaryen's optimization of the
    # linked module, which takes minutes and hardly speeds the tests up (the
    # objects are optimized already).
    target_link_options(tableskim-tests PRIVATE ${TABLESKIM_WASM_LINK_OPTIONS} -sNODERAWFS=1
        $<$<NOT:$<CONFIG:Debug>>:-O1>)
    add_test(NAME tableskim-tests COMMAND tableskim-tests)
    set_tests_properties(tableskim-tests PROPERTIES TIMEOUT 1200)
else()
    # One ctest test per test case, so that `ctest -j` runs them in parallel and
    # a hung one times out alone. Listed when ctest runs (PRE_TEST), not after
    # every link. Hidden tests ([.super-large]) are not listed.
    include(${catch2_SOURCE_DIR}/extras/Catch.cmake)
    catch_discover_tests(tableskim-tests
        DISCOVERY_MODE PRE_TEST
        PROPERTIES TIMEOUT 300
    )
endif()

# End-to-end tests of the terminal UI (tests/tui/), which run tableskim -t in a
# pseudo-terminal: Linux only, with Python 3 and pyte. AUTO adds them when
# pyte is found, ON requires it (CI); -DPython3_EXECUTABLE= picks a venv.
set(TABLESKIM_TUI_TESTS AUTO CACHE STRING "End-to-end tests of the terminal UI (AUTO, ON or OFF; Linux only)")
set_property(CACHE TABLESKIM_TUI_TESTS PROPERTY STRINGS AUTO ON OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT TABLESKIM_TUI_TESTS STREQUAL "OFF")
    find_package(Python3 COMPONENTS Interpreter)
    set(TABLESKIM_HAVE_PYTE FALSE)
    if(Python3_Interpreter_FOUND)
        execute_process(COMMAND ${Python3_EXECUTABLE} -I -c "import pyte"
            RESULT_VARIABLE TABLESKIM_PYTE_RESULT OUTPUT_QUIET ERROR_QUIET)
        if(TABLESKIM_PYTE_RESULT EQUAL 0)
            set(TABLESKIM_HAVE_PYTE TRUE)
        endif()
    endif()
    if(TABLESKIM_HAVE_PYTE)
        add_test(NAME tableskim-tui-tests
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/tui/test_tui.py
                --tableskim $<TARGET_FILE:tableskim>
                --demo-data ${CMAKE_CURRENT_SOURCE_DIR}/demo/data)
        set_tests_properties(tableskim-tui-tests PROPERTIES TIMEOUT 600)
    elseif(TABLESKIM_TUI_TESTS STREQUAL "ON")
        message(FATAL_ERROR "TABLESKIM_TUI_TESTS=ON needs Python 3 with pyte (python3-pyte, or pip install -r tests/tui/requirements.txt)")
    else()
        message(STATUS "Terminal UI tests skipped: Python 3 with pyte not found")
    endif()
endif()

# The web app's lint (ESLint) and formatting (Prettier) checks.
if(EMSCRIPTEN AND TABLESKIM_HAVE_WEB_APP)
    add_test(NAME tableskim-web-lint COMMAND ${TABLESKIM_NPM} run lint
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/web)
    add_test(NAME tableskim-web-format COMMAND ${TABLESKIM_NPM} run format:check
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/web)
endif()

# End-to-end tests of the web app (web/tests/) in headless Chromium, with
# Node.js and Playwright (a package of web/, which npm ci installs; the
# browser comes from npx playwright install chromium): WebAssembly builds
# with the web app only. AUTO adds them when Playwright is found at
# configure time, ON requires it, as CI does.
set(TABLESKIM_WEB_TESTS AUTO CACHE STRING "End-to-end tests of the web app (AUTO, ON or OFF; WebAssembly builds only)")
set_property(CACHE TABLESKIM_WEB_TESTS PROPERTY STRINGS AUTO ON OFF)
if(EMSCRIPTEN AND TABLESKIM_HAVE_WEB_APP AND NOT TABLESKIM_WEB_TESTS STREQUAL "OFF")
    find_program(TABLESKIM_NODE node)
    set(TABLESKIM_HAVE_PLAYWRIGHT FALSE)
    if(TABLESKIM_NODE)
        execute_process(COMMAND ${TABLESKIM_NODE} -e "require('playwright')"
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/web
            RESULT_VARIABLE TABLESKIM_PLAYWRIGHT_RESULT OUTPUT_QUIET ERROR_QUIET)
        if(TABLESKIM_PLAYWRIGHT_RESULT EQUAL 0)
            set(TABLESKIM_HAVE_PLAYWRIGHT TRUE)
        endif()
    endif()
    if(TABLESKIM_HAVE_PLAYWRIGHT)
        add_test(NAME tableskim-web-tests
            COMMAND ${TABLESKIM_NODE} --test ${CMAKE_CURRENT_SOURCE_DIR}/web/tests/app.test.cjs)
        # Chromium drops non-ASCII paths given to a file input without a
        # UTF-8 locale.
        set_tests_properties(tableskim-web-tests PROPERTIES TIMEOUT 600
            ENVIRONMENT "TABLESKIM_WASM_BUILD=${CMAKE_CURRENT_BINARY_DIR};LANG=C.UTF-8")
    elseif(TABLESKIM_WEB_TESTS STREQUAL "ON")
        message(FATAL_ERROR "TABLESKIM_WEB_TESTS=ON needs Node.js with Playwright (npm ci in web/)")
    else()
        message(STATUS "Web app tests skipped: Node.js with Playwright not found")
    endif()
endif()

# Tests that drive the GUI in-process (tests/gui/): Linux only, under Xvfb
# (xvfb-run). AUTO adds them when xvfb-run is found, ON requires it (CI).
set(TABLESKIM_GUI_TESTS AUTO CACHE STRING "Tests of the GUI under Xvfb (AUTO, ON or OFF; Linux only)")
set_property(CACHE TABLESKIM_GUI_TESTS PROPERTY STRINGS AUTO ON OFF)
if(TABLESKIM_BUILD_GUI AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT TABLESKIM_GUI_TESTS STREQUAL "OFF")
    find_program(TABLESKIM_XVFB_RUN xvfb-run)
    if(TABLESKIM_XVFB_RUN)
        add_executable(tableskim-gui-tests
            tests/gui/gui_test_main.cxx
            tests/gui/gui_test_helpers.hxx
            tests/gui/gui_test_helpers.cxx
            tests/gui/gui_window_tests.cxx
        )
        target_link_libraries(tableskim-gui-tests PRIVATE tableskim-gui Catch2::Catch2 tableskim-warnings)
        target_compile_definitions(tableskim-gui-tests PRIVATE
            TABLESKIM_DEMO_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/demo/data"
        )
        add_test(NAME tableskim-gui-tests
            COMMAND ${TABLESKIM_XVFB_RUN} -a -s "-screen 0 1600x1200x24" $<TARGET_FILE:tableskim-gui-tests>)
        set_tests_properties(tableskim-gui-tests PROPERTIES TIMEOUT 600)
    elseif(TABLESKIM_GUI_TESTS STREQUAL "ON")
        message(FATAL_ERROR "TABLESKIM_GUI_TESTS=ON needs xvfb-run (the xvfb package)")
    else()
        message(STATUS "GUI tests skipped: xvfb-run not found")
    endif()
endif()

if(NOT EMSCRIPTEN)
    include(cmake/Packaging.cmake)
endif()
