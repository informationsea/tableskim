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

# Progress: numbered steps with the time each took.
if [ -t 1 ]; then
    BOLD=$'\e[1m'
    DIM=$'\e[2m'
    RESET=$'\e[0m'
else
    BOLD=
    DIM=
    RESET=
fi
STEPS=1 # serving
[ "${BUILD}" = 1 ] && STEPS=$((STEPS + 3))
[ "${DEV}" = 1 ] && STEPS=$((STEPS + 1))
STEP=0
STEP_START=${SECONDS}

duration()
{
    if [ "$1" -ge 60 ]; then
        printf '%dm%02ds' $(($1 / 60)) $(($1 % 60))
    else
        printf '%ds' "$1"
    fi
}

step()
{
    STEP=$((STEP + 1))
    STEP_START=${SECONDS}
    echo "${BOLD}==> [${STEP}/${STEPS}] $*${RESET}"
}

note()
{
    echo "    ${DIM}$*${RESET}"
}

step_done()
{
    note "done in $(duration $((SECONDS - STEP_START)))"
}

mebibytes()
{
    awk -v bytes="$(wc -c < "$1")" 'BEGIN { printf "%.1f MiB", bytes / 1048576 }'
}

for tool in node npm; do
    if ! command -v "${tool}" &> /dev/null; then
        echo "${tool} not found: install Node.js (with npm)" >&2
        exit 1
    fi
done

if [ "${BUILD}" = 1 ]; then
    step "Setting up Emscripten"
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
    note "$(emcc --version 2> /dev/null | head -n 1) in ${EMSDK}"

    # Configure once (as build.sh does), and again if the web app was left
    # out of an earlier configure (TABLESKIM_WEB_APP).
    step "Configuring ${BUILD_DIR}"
    if [ ! -f "${BUILD_DIR}/CMakeCache.txt" ]; then
        note "first time: CMake downloads DuckDB and the other dependencies (a few minutes)"
        cmake --preset "${PRESET}" -DTABLESKIM_WEB_APP=ON -DFETCHCONTENT_QUIET=OFF
        step_done
    elif grep -qE '^TABLESKIM_WEB_APP:STRING=OFF|^TABLESKIM_NPM:FILEPATH=.*NOTFOUND' "${BUILD_DIR}/CMakeCache.txt"; then
        note "again, with the web app"
        cmake --preset "${PRESET}" -DTABLESKIM_WEB_APP=ON
        step_done
    else
        note "configured already"
    fi

    if [ "${DEV}" = 1 ]; then
        # Vite builds the app itself; only the module is needed.
        TARGET=tableskim-wasm
        step "Building the WebAssembly module"
    else
        TARGET=tableskim-web
        step "Building the WebAssembly module and the web app"
    fi
    if [ ! -f "${BUILD_DIR}/tableskim-wasm.wasm" ]; then
        note "first build: DuckDB and the other dependencies too (tens of minutes)"
    fi
    if [ "${BUILD_TYPE}" = release ]; then
        note "linking the module (optimized) can take minutes with no output"
    fi
    # Ninja's status line: steps done/total, percentage, seconds so far.
    NINJA_STATUS="[%f/%t %p %es] " cmake --build --preset "${PRESET}" --target "${TARGET}"
    step_done
    note "module: ${BUILD_DIR}/tableskim-wasm.wasm ($(mebibytes "${BUILD_DIR}/tableskim-wasm.wasm"))"
fi

if [ "${DEV}" = 1 ]; then
    if [ ! -f "${BUILD_DIR}/tableskim-wasm.wasm" ]; then
        echo "${BUILD_DIR}/tableskim-wasm.wasm not found: run without --no-build first" >&2
        exit 1
    fi
    cd web
    step "Installing the web app's packages"
    if [ ! -d node_modules ] || [ package-lock.json -nt node_modules/.package-lock.json ]; then
        npm ci --no-audit --no-fund
        step_done
    else
        note "installed already"
    fi
    args=(--host 127.0.0.1)
    [ -n "${PORT}" ] && args+=(--port "${PORT}" --strictPort)
    [ "${OPEN}" = 1 ] && args+=(--open)
    step "Serving web/ with Vite's development server (Ctrl+C stops it)"
    [ "${BUILD}" = 1 ] && note "ready after $(duration "${SECONDS}")"
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
step "Serving the web app of ${BUILD_DIR} (Ctrl+C stops it)"
[ "${BUILD}" = 1 ] && note "ready after $(duration "${SECONDS}")"
note "open ${URL} in a browser"
exec node web/serve.mjs "${BUILD_DIR}" "${PORT}"
