#!/bin/bash
#
# Usage: ./build.sh [debug|release]   (default: release)
#
# Configures and builds with the presets in CMakePresets.json.

set -e

BUILD_TYPE="${1:-release}"

case "${BUILD_TYPE}" in
    debug|release) ;;
    *)
        echo "Usage: $0 [debug|release]" >&2
        exit 1
        ;;
esac

# check ninja is installed
if command -v ninja &> /dev/null
then
    PRESET="${BUILD_TYPE}"
else
    PRESET="make-${BUILD_TYPE}"
fi

cd "$(dirname "$0")"

# A failed configure can leave CMakeCache.txt without build files. Retry
# configuration each time, reusing the existing cache and dependencies.
cmake --preset "${PRESET}"
cmake --build --preset "${PRESET}"
ctest --preset "${PRESET}"
