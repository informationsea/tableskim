#!/bin/bash
#
# Usage: ./web.sh [--debug] [--dev] [--port PORT] [--open] [--no-build]
#
# Builds the WebAssembly core and the web app (web/) with the wasm-release
# preset (wasm-debug with --debug) and serves them at http://127.0.0.1:PORT/
# (8765 by default) until Ctrl+C. See src/wasm/README.md.
#
#   --dev       serve web/ with Vite's development server instead, which
#               reloads the page as web/src changes (port 5173 by default)
#   --port      the port to listen on
#   --open      open the page in the browser
#   --no-build  serve what was built last
#
# Needs the Emscripten SDK (its emsdk_env.sh is sourced from $EMSDK or
# ~/emsdk when emcc is not on the PATH), Node.js with npm, CMake and Ninja.
# The first build compiles DuckDB and the other dependencies: it takes long.

set -e

BUILD_TYPE=release
DEV=0
PORT=
OPEN=0
BUILD=1

usage()
{
    sed -n '3,14p' "$0" | sed 's/^# \{0,1\}//' >&2
    exit "${1:-1}"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --debug) BUILD_TYPE=debug ;;
        --dev) DEV=1 ;;
        --port)
            [ $# -ge 2 ] || usage
            PORT="$2"
            shift
            ;;
        --open) OPEN=1 ;;
        --no-build) BUILD=0 ;;
        -h|--help) usage 0 ;;
        *)
            echo "Unknown option: $1" >&2
            usage
            ;;
    esac
    shift
done

cd "$(dirname "$0")"

PRESET="wasm-${BUILD_TYPE}"
BUILD_DIR="build/${PRESET}"

for tool in node npm; do
    if ! command -v "${tool}" &> /dev/null; then
        echo "${tool} not found: install Node.js (with npm)" >&2
        exit 1
    fi
done

if [ "${BUILD}" = 1 ]; then
    # emsdk_env.sh puts emcc on the PATH and sets EMSDK, through which the
    # preset finds Emscripten's CMake toolchain file.
    if ! command -v emcc &> /dev/null || [ -z "${EMSDK:-}" ]; then
        for env in "${EMSDK:-}/emsdk_env.sh" "${HOME}/emsdk/emsdk_env.sh"; do
            if [ -f "${env}" ]; then
                # shellcheck disable=SC1090
                source "${env}" > /dev/null 2>&1
                break
            fi
        done
    fi
    if ! command -v emcc &> /dev/null || [ ! -f "${EMSDK:-}/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake" ]; then
        echo "The Emscripten SDK was not found: install it (https://emscripten.org/docs/getting_started/downloads.html)," >&2
        echo "then source <emsdk>/emsdk_env.sh or set EMSDK to its directory." >&2
        exit 1
    fi

    # Configure once (as build.sh does), and again if the web app was left
    # out of an earlier configure (TABLESKIM_WEB_APP).
    if [ ! -f "${BUILD_DIR}/CMakeCache.txt" ] ||
        grep -qE '^TABLESKIM_WEB_APP:STRING=OFF|^TABLESKIM_NPM:FILEPATH=.*NOTFOUND' "${BUILD_DIR}/CMakeCache.txt"; then
        cmake --preset "${PRESET}" -DTABLESKIM_WEB_APP=ON
    fi
    if [ "${DEV}" = 1 ]; then
        # Vite builds the app itself; only the module is needed.
        cmake --build --preset "${PRESET}" --target tableskim-wasm
    else
        cmake --build --preset "${PRESET}" --target tableskim-web
    fi
fi

if [ "${DEV}" = 1 ]; then
    if [ ! -f "${BUILD_DIR}/tableskim-wasm.wasm" ]; then
        echo "${BUILD_DIR}/tableskim-wasm.wasm not found: run without --no-build first" >&2
        exit 1
    fi
    cd web
    if [ ! -d node_modules ] || [ package-lock.json -nt node_modules/.package-lock.json ]; then
        npm ci --no-audit --no-fund
    fi
    args=(--host 127.0.0.1)
    [ -n "${PORT}" ] && args+=(--port "${PORT}" --strictPort)
    [ "${OPEN}" = 1 ] && args+=(--open)
    TABLESKIM_WASM_BUILD="../${BUILD_DIR}" exec npm run dev -- "${args[@]}"
fi

if [ ! -f "${BUILD_DIR}/web/index.html" ]; then
    echo "${BUILD_DIR}/web/index.html not found: run without --no-build first" >&2
    exit 1
fi
PORT="${PORT:-8765}"
URL="http://127.0.0.1:${PORT}/"
if [ "${OPEN}" = 1 ]; then
    opener=
    if command -v xdg-open &> /dev/null; then
        opener=xdg-open
    elif command -v open &> /dev/null; then
        opener=open
    fi
    if [ -n "${opener}" ]; then
        # Once the server below is listening.
        (sleep 1 && "${opener}" "${URL}" > /dev/null 2>&1) &
    fi
fi
echo "Serving the web app of ${BUILD_DIR} at ${URL} (Ctrl+C stops it)"
exec node web/serve.mjs "${BUILD_DIR}" "${PORT}"
