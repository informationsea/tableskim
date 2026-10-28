# Packs the built web app into a ZIP to put on any web server (the
# release's tableskim-<version>-web.zip), run by the tableskim-web-zip
# target with cmake -P:
#
#   APP_DIR         the built app (<build>/web)
#   STAGE_DIR       the directory to fill, named as the ZIP's top folder
#   ZIP_FILE        the ZIP to write
#   LICENSE_FILE    TableSkim's LICENSE
#   THIRD_PARTY_FILE  THIRD_PARTY_LICENSES.txt of the build (cmake/Licenses.cmake)
#   WEB_SOURCE_DIR  web/, whose node_modules hold the bundled packages
#   EMSCRIPTEN_LICENSE  Emscripten's LICENSE (its runtime is in the module)
#
# The app is bundled with the packages it depends on at run time, so their
# licenses are added to the build's list: the native list does not have them.
foreach(var APP_DIR STAGE_DIR ZIP_FILE LICENSE_FILE THIRD_PARTY_FILE WEB_SOURCE_DIR EMSCRIPTEN_LICENSE)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "WebZip.cmake: ${var} is not set")
    endif()
endforeach()
if(NOT EXISTS "${APP_DIR}/index.html")
    message(FATAL_ERROR "WebZip.cmake: the web app is not built in ${APP_DIR}")
endif()

file(REMOVE_RECURSE "${STAGE_DIR}")
file(MAKE_DIRECTORY "${STAGE_DIR}")
file(COPY "${APP_DIR}/" DESTINATION "${STAGE_DIR}")
file(COPY "${LICENSE_FILE}" DESTINATION "${STAGE_DIR}")

file(READ "${THIRD_PARTY_FILE}" licenses)
set(rule "================================================================================")
foreach(entry
        "Emscripten (runtime in tableskim-wasm.js and .wasm)|${EMSCRIPTEN_LICENSE}"
        "React|${WEB_SOURCE_DIR}/node_modules/react/LICENSE"
        "React DOM|${WEB_SOURCE_DIR}/node_modules/react-dom/LICENSE"
        "scheduler (used by React DOM)|${WEB_SOURCE_DIR}/node_modules/scheduler/LICENSE")
    string(FIND "${entry}" "|" bar REVERSE)
    string(SUBSTRING "${entry}" 0 ${bar} label)
    math(EXPR start "${bar} + 1")
    string(SUBSTRING "${entry}" ${start} -1 path)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "WebZip.cmake: license of ${label} not found: ${path}")
    endif()
    file(READ "${path}" body)
    string(REPLACE "\r\n" "\n" body "${body}")
    string(APPEND licenses "${rule}\n${label}\n${rule}\n\n${body}\n\n")
endforeach()
file(WRITE "${STAGE_DIR}/THIRD_PARTY_LICENSES.txt" "${licenses}")

file(WRITE "${STAGE_DIR}/README.txt"
"TableSkim for the web browser
=============================

TableSkim, running in the browser: the files you open are read there and
are never uploaded. See https://github.com/informationsea/tableskim.

To use it, put this folder on any web server, under any path, and open the
folder's URL with the slash at the end (https://example.com/tools/tableskim/).
It needs HTTPS, or http://localhost, and a recent browser (it is tested in
Chromium); opening index.html as a file does not work.

The core runs on several threads, which browsers allow only in a
cross-origin isolated page. Servers that can should send these headers with
every file of the folder:

    Cross-Origin-Opener-Policy: same-origin
    Cross-Origin-Embedder-Policy: require-corp

Where they cannot be set (GitHub Pages, for one), coi-serviceworker.js adds
them: the page registers it, for this folder only, and reloads once.

To try it on your own computer:

    python3 -m http.server 8000

in this folder, then open http://localhost:8000/.
")

get_filename_component(stage_parent "${STAGE_DIR}" DIRECTORY)
get_filename_component(stage_name "${STAGE_DIR}" NAME)
file(REMOVE "${ZIP_FILE}")
execute_process(
    COMMAND ${CMAKE_COMMAND} -E tar cf "${ZIP_FILE}" --format=zip "${stage_name}"
    WORKING_DIRECTORY "${stage_parent}"
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "WebZip.cmake: cannot write ${ZIP_FILE}")
endif()
message(STATUS "Web app packed into ${ZIP_FILE}")
