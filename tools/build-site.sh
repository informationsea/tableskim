#!/bin/sh
# Builds the GitHub Pages site: the landing pages in site/ and the user
# manual in docs/ converted to HTML by tableskim-html-manual, so the manual
# online is the one built into the GUI and installed with the packages.
#
#   tools/build-site.sh [output directory [release.json]]
#
# The output directory defaults to build/site. With release.json (the GitHub
# API's answer for the latest release), the download pages link to its files
# (tools/fill-download-pages.py); without it, to the releases page.
#
# Only the converter is compiled (with $CXX, default c++), as it needs none
# of the dependencies; a full CMake configure would fetch them all.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
output=${1:-"$root/build/site"}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

"${CXX:-c++}" -std=c++20 -O2 -I"$root/src/common" \
    "$root/src/tools/html_manual.cxx" \
    "$root/src/common/markdown.cxx" \
    "$root/src/common/help_docs.cxx" \
    -o "$work/tableskim-html-manual"

rm -rf "$output"
mkdir -p "$output"
cp -R "$root/site/." "$output/"
cp "$root/src/images/icon.png" "$output/icon.png"
"$work/tableskim-html-manual" "$root/docs" "$output/manual"
# Pages would otherwise run the files through Jekyll.
touch "$output/.nojekyll"
if [ $# -ge 2 ]; then
    python3 "$root/tools/fill-download-pages.py" "$2" "$output"
fi
echo "Site written to $output"
