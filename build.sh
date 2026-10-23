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
    BUILD_DIR="build/${BUILD_TYPE}"
else
    PRESET="make-${BUILD_TYPE}"
    BUILD_DIR="build/make-${BUILD_TYPE}"
fi

cd "$(dirname "$0")"

# Test the cache, not the directory: an interrupted first configure leaves a
# directory that was never configured.
if [ ! -f "${BUILD_DIR}/CMakeCache.txt" ]; then
    cmake --preset "${PRESET}"
fi
cmake --build --preset "${PRESET}"
ctest --preset "${PRESET}"
