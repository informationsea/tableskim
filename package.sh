#!/bin/bash
#
# Usage: ./package.sh [deb|rpm|dmg|zip|all]   (default: all for this OS)
#
# Builds a release binary and creates distributable packages in the build
# directory.
#   Linux : .deb (dpkg-dev), .rpm (rpmbuild), .zip
#   macOS : .dmg, .zip
# On Windows use package.bat.
#
# The preset is release (make-release without Ninja) unless TABLESKIM_PRESET
# is set, e.g. to macos-x86_64-release for Intel Macs.

set -e

KIND="${1:-all}"
cd "$(dirname "$0")"

if [ -n "${TABLESKIM_PRESET}" ]; then
    BUILD_DIR="build/${TABLESKIM_PRESET}"
    cmake --preset "${TABLESKIM_PRESET}"
    cmake --build --preset "${TABLESKIM_PRESET}"
    ctest --preset "${TABLESKIM_PRESET}"
else
    ./build.sh release
    if command -v ninja &> /dev/null; then
        BUILD_DIR="build/release"
    else
        BUILD_DIR="build/make-release"
    fi
fi

case "$(uname -s)" in
    Darwin) ALL="DragNDrop ZIP" ;;
    *)      ALL="DEB RPM ZIP" ;;
esac

case "${KIND}" in
    deb) GENERATORS="DEB" ;;
    rpm) GENERATORS="RPM" ;;
    dmg) GENERATORS="DragNDrop" ;;
    zip) GENERATORS="ZIP" ;;
    all) GENERATORS="${ALL}" ;;
    *) echo "Usage: $0 [deb|rpm|dmg|zip|all]" >&2; exit 1 ;;
esac

cd "${BUILD_DIR}"
for g in ${GENERATORS}; do
    cpack -G "${g}"
done
ls -l ./*.deb ./*.rpm ./*.dmg ./*.zip 2>/dev/null || true
