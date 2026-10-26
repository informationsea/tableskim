# Packaging via CPack.
#
#   Linux   : DEB, RPM, ZIP
#   macOS   : DragNDrop (.dmg), ZIP
#   Windows : NSIS installer (.exe), ZIP
#
# Only targets installed under the "runtime" component are packaged, so the
# install rules of FetchContent dependencies (FLTK, DuckDB, ...) are ignored.
#
#   cmake --build build/release
#   cd build/release && cpack -G DEB    # or RPM / ZIP / DragNDrop / NSIS
# or simply ./package.sh (Linux/macOS) / package.bat (Windows).

include(GNUInstallDirs)

if(UNIX AND NOT APPLE)
    # All third-party code is linked statically; drop the build-tree RUNPATH
    # that FLTK leaks into the executables so installed binaries carry none.
    set(CMAKE_SKIP_RPATH ON)

    install(TARGETS tableskim
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        COMPONENT runtime
    )
    if(TABLESKIM_BUILD_GUI)
        install(FILES packaging/tableskim.desktop
            DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
            COMPONENT runtime
        )
        foreach(size ${TABLESKIM_ICON_SIZES})
            install(FILES ${CMAKE_CURRENT_BINARY_DIR}/icons/${size}x${size}/tableskim.png
                DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps
                COMPONENT runtime
            )
        endforeach()
        # File icons only for the formats defined in tableskim-mime.xml;
        # CSV, Excel, SQLite and JSON files keep the icons of the user's theme.
        # Package triggers of shared-mime-info and the icon cache pick these up.
        install(FILES packaging/linux/tableskim-mime.xml
            DESTINATION ${CMAKE_INSTALL_DATADIR}/mime/packages
            RENAME tableskim.xml
            COMPONENT runtime
        )
        foreach(pair "parquet;application-vnd.apache.parquet" "duckdb;application-x-duckdb"
                     "json;application-x-ndjson" "vcf;text-x-vcf")
            list(GET pair 0 format)
            list(GET pair 1 icon)
            install(FILES src/images/filetypes/${format}.svg
                DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/mimetypes
                RENAME ${icon}.svg
                COMPONENT runtime
            )
            install(FILES src/images/filetypes/${format}-16.svg
                DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/16x16/mimetypes
                RENAME ${icon}.svg
                COMPONENT runtime
            )
        endforeach()
    endif()
    install(FILES LICENSE ${TABLESKIM_LICENSE_FILE}
        DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/tableskim
        COMPONENT runtime
    )
    install(DIRECTORY ${TABLESKIM_MANUAL_DIR}/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/tableskim/html
        COMPONENT runtime
    )
elseif(APPLE)
    # TableSkim.app only; its executable also runs the TUI (see README.md).
    # Without the GUI, the plain tableskim executable.
    # COMPONENT applies only to the artifact kind it follows, so each needs one.
    install(TARGETS tableskim
        BUNDLE DESTINATION . COMPONENT runtime
        RUNTIME DESTINATION . COMPONENT runtime
    )
    # An ad-hoc signature (no certificate): Apple Silicon refuses to run
    # unsigned code, and a download without one is reported as "damaged". It
    # does not satisfy Gatekeeper, which still asks the user to allow the app
    # once; that needs a Developer ID signature and notarization.
    install(CODE "
        set(_dest \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}\")
        file(GLOB _apps \"\${_dest}/*.app\")
        if(NOT _apps)
            set(_apps \"\${_dest}/tableskim\")
        endif()
        foreach(_app IN LISTS _apps)
            execute_process(COMMAND codesign --force --deep --sign - \"\${_app}\"
                RESULT_VARIABLE _result)
            if(NOT _result EQUAL 0)
                message(FATAL_ERROR \"codesign failed for \${_app}\")
            endif()
        endforeach()
    " COMPONENT runtime)
    install(FILES LICENSE ${TABLESKIM_LICENSE_FILE} DESTINATION . COMPONENT runtime)
    install(DIRECTORY ${TABLESKIM_MANUAL_DIR}/ DESTINATION Manual COMPONENT runtime)
elseif(WIN32)
    install(TARGETS tableskim
        RUNTIME DESTINATION .
        COMPONENT runtime
    )
    if(TARGET tableskim-tui)
        # Console-subsystem terminal UI (TableSkim.exe has no console)
        install(TARGETS tableskim-tui
            RUNTIME DESTINATION .
            COMPONENT runtime
        )
    endif()
    install(FILES LICENSE ${TABLESKIM_LICENSE_FILE} DESTINATION . COMPONENT runtime)
    install(DIRECTORY ${TABLESKIM_MANUAL_DIR}/ DESTINATION Manual COMPONENT runtime)

    # MSVC runtime DLLs (the build links the dynamic runtime).
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION .)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_COMPONENT runtime)
    include(InstallRequiredSystemLibraries)
endif()

if(TABLESKIM_BUILD_GUI)
    set(CPACK_PACKAGE_NAME "tableskim")
    set(_tableskim_other_package "tableskim-tui")
else()
    # The terminal UI only: a package of its own, as both install bin/tableskim.
    set(CPACK_PACKAGE_NAME "tableskim-tui")
    set(_tableskim_other_package "tableskim")
endif()
set(CPACK_DEBIAN_PACKAGE_CONFLICTS "${_tableskim_other_package}")
set(CPACK_RPM_PACKAGE_CONFLICTS "${_tableskim_other_package}")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VENDOR "Yasunobu Okamura")
set(CPACK_PACKAGE_CONTACT "Yasunobu Okamura <okamura@informationsea.info>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "A lightweight viewer for tabular files")
set(_tableskim_formats "CSV, TSV and VCF files (optionally gzip, bzip2, xz or zstd compressed), Excel workbooks, Parquet, JSON / NDJSON, DuckDB and SQLite databases")
if(TABLESKIM_BUILD_GUI)
    set(CPACK_PACKAGE_DESCRIPTION
        "TableSkim is a lightweight viewer for ${_tableskim_formats}, with a graphical and a terminal user interface.")
else()
    set(CPACK_PACKAGE_DESCRIPTION
        "TableSkim is a lightweight viewer for ${_tableskim_formats}. This package has the terminal user interface only.")
endif()
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")

# One package containing just the runtime component.
set(CPACK_COMPONENTS_ALL runtime)
set(CPACK_COMPONENTS_GROUPING ALL_COMPONENTS_IN_ONE)
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_RPM_COMPONENT_INSTALL ON)
set(CPACK_STRIP_FILES ON)

# DEB and RPM packages run only on distributions as new as the one they
# were built on, so their names tell which one that was (e.g. ubuntu24.04).
# The ZIP is built on the oldest supported one (Rocky Linux 8) for all of
# them, so its name has none.
# -DTABLESKIM_PACKAGE_OS_TAG=... overrides ID and VERSION_ID of
# /etc/os-release; read on every configure, so it follows an upgrade.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT DEFINED TABLESKIM_PACKAGE_OS_TAG)
    cmake_host_system_information(RESULT _tableskim_distrib_id QUERY DISTRIB_ID)
    cmake_host_system_information(RESULT _tableskim_distrib_version QUERY DISTRIB_VERSION_ID)
    set(TABLESKIM_PACKAGE_OS_TAG "${_tableskim_distrib_id}${_tableskim_distrib_version}")
endif()

# Archive names: tableskim-<version>-<system>[-x11]-<arch>.zip, -x11 for the
# GUI without Wayland (TABLESKIM_X11_ONLY), built next to the default one.
set(_tableskim_variant "")
if(TABLESKIM_X11_ONLY)
    set(_tableskim_variant "-x11")
endif()
set(CPACK_PACKAGE_FILE_NAME
    "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-${CMAKE_SYSTEM_NAME}${_tableskim_variant}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)

# DEB: tableskim_<version>_<distribution>_<arch>.deb
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
find_program(TABLESKIM_DPKG_EXECUTABLE dpkg)
if(TABLESKIM_PACKAGE_OS_TAG AND TABLESKIM_DPKG_EXECUTABLE)
    execute_process(COMMAND ${TABLESKIM_DPKG_EXECUTABLE} --print-architecture
        OUTPUT_VARIABLE _tableskim_deb_arch OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_tableskim_deb_arch)
        set(CPACK_DEBIAN_FILE_NAME
            "${CPACK_PACKAGE_NAME}_${CPACK_PACKAGE_VERSION}_${TABLESKIM_PACKAGE_OS_TAG}_${_tableskim_deb_arch}.deb")
    endif()
endif()
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)

# RPM
set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_PACKAGE_GROUP "Applications/Productivity")
# CPackRPM does not fall back to CPACK_PACKAGE_DESCRIPTION, but to CMake's
# generic CPACK_PACKAGE_DESCRIPTION_FILE.
set(CPACK_RPM_PACKAGE_DESCRIPTION "${CPACK_PACKAGE_DESCRIPTION}")
set(CPACK_RPM_PACKAGE_LICENSE "AGPL-3.0-only")
set(CPACK_RPM_PACKAGE_AUTOREQ ON)
set(CPACK_RPM_PACKAGE_RELEASE 1)
# RPM: tableskim-<version>-<release>.<distribution>.<arch>.rpm
if(TABLESKIM_PACKAGE_OS_TAG)
    set(CPACK_RPM_FILE_NAME
        "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-${CPACK_RPM_PACKAGE_RELEASE}.${TABLESKIM_PACKAGE_OS_TAG}.${CMAKE_SYSTEM_PROCESSOR}.rpm")
endif()
# Do not claim ownership of system directories.
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
    /usr/share/applications
    /usr/share/icons
    /usr/share/icons/hicolor
    /usr/share/icons/hicolor/scalable
    /usr/share/icons/hicolor/scalable/mimetypes
    /usr/share/icons/hicolor/16x16/mimetypes
    /usr/share/mime
    /usr/share/mime/packages
)
foreach(size ${TABLESKIM_ICON_SIZES})
    list(APPEND CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
        /usr/share/icons/hicolor/${size}x${size}
        /usr/share/icons/hicolor/${size}x${size}/apps
    )
endforeach()

# DMG
set(CPACK_DMG_VOLUME_NAME "TableSkim")
set(CPACK_DMG_FORMAT "UDZO")

# NSIS (Windows installer)
set(CPACK_NSIS_DISPLAY_NAME "TableSkim")
set(CPACK_NSIS_PACKAGE_NAME "TableSkim")
set(CPACK_NSIS_INSTALL_ROOT "$PROGRAMFILES64")
set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
set(CPACK_NSIS_MODIFY_PATH OFF)
set(CPACK_NSIS_URL_INFO_ABOUT "https://github.com/informationsea/tableskim")
# NSIS wants backslashes in paths.
file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/src/images/icon.ico" _tableskim_ico)
string(REPLACE "\\" "\\\\" _tableskim_ico "${_tableskim_ico}")
set(CPACK_NSIS_MUI_ICON "${_tableskim_ico}")
set(CPACK_NSIS_MUI_UNIICON "${_tableskim_ico}")
set(CPACK_NSIS_INSTALLED_ICON_NAME "TableSkim.exe")
# Start menu shortcut for the GUI (executable is renamed to TableSkim.exe).
set(CPACK_PACKAGE_EXECUTABLES "TableSkim" "TableSkim")
# ... and for the manual.
set(CPACK_NSIS_MENU_LINKS "Manual/README.html" "TableSkim User Manual")
# The executable is installed at the top level, not in the default "bin".
set(CPACK_NSIS_EXECUTABLES_DIRECTORY ".")
# File type registration lives in .nsh files to avoid escaping NSIS quotes here.
file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/register_file_types.nsh" _tableskim_register_nsh)
file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/unregister_file_types.nsh" _tableskim_unregister_nsh)
string(REPLACE "\\" "\\\\" _tableskim_register_nsh "${_tableskim_register_nsh}")
string(REPLACE "\\" "\\\\" _tableskim_unregister_nsh "${_tableskim_unregister_nsh}")
set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS "!include '${_tableskim_register_nsh}'")
set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS "!include '${_tableskim_unregister_nsh}'")

# Per-generator settings that cannot be expressed as plain variables.
set(CPACK_PROJECT_CONFIG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CPackGeneratorConfig.cmake")

include(CPack)
