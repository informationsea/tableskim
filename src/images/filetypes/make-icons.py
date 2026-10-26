#!/usr/bin/env python3
"""Generates the file type icons.

Writes <name>.svg (32 px and up), <name>-16.svg (pixel-fitted for 16 and
24 px), <name>.ico (Windows) and <name>.icns (macOS) for every format
below. Linux installs the SVGs directly. Needs resvg-py:

    python3 -m venv .venv && .venv/bin/pip install resvg-py
    .venv/bin/python make-icons.py

Labels are drawn as strokes rather than text so the output does not depend
on the fonts installed on the machine.
"""

import struct
from pathlib import Path

import resvg_py

HERE = Path(__file__).resolve().parent

# name, label, band color, label color
FORMATS = [
    ("csv", "CSV", "#0F8A5F", "#FFFFFF"),
    ("tsv", "TSV", "#7A4FD0", "#FFFFFF"),
    ("xlsx", "XLSX", "#1D6F42", "#FFFFFF"),
    ("xlsm", "XLSM", "#1D6F42", "#FFFFFF"),
    ("parquet", "PARQ", "#D9650B", "#FFFFFF"),
    ("duckdb", "DUCK", "#E3B505", "#3A2E00"),
    ("sqlite", "SQL", "#0B5F8C", "#FFFFFF"),
    ("json", "JSON", "#4A4A4A", "#FFFFFF"),
    ("vcf", "VCF", "#B0306A", "#FFFFFF"),
]

LARGE_SIZES = [256, 64, 48, 32]
SMALL_SIZES = [24, 16]
# ICNS element types with PNG data, and their pixel sizes; "@2x" types are
# the Retina variants of the next smaller point size.
ICNS_TYPES = [
    (b"icp5", 32), (b"ic11", 32), (b"ic12", 64), (b"ic07", 128),
    (b"ic13", 256), (b"ic08", 256), (b"ic14", 512), (b"ic09", 512),
]

# Stroke glyphs on an 8 x 12 cell (M is 9 wide).
GLYPHS = {
    "A": (8, "M1 11L4 1L7 11M2.2 7.5H5.8"),
    "C": (8, "M7 1.5H3a2 2 0 0 0-2 2v5a2 2 0 0 0 2 2h4"),
    "D": (8, "M1.5 1.5h3a2.5 2.5 0 0 1 2.5 2.5v4a2.5 2.5 0 0 1-2.5 2.5h-3Z"),
    "F": (8, "M7 1.5H1.5V11M1.5 6.2H5.8"),
    "J": (8, "M2.5 1.5H7M5.5 1.5V8.5a2 2 0 0 1-2 2h-0.5a2 2 0 0 1-2-2"),
    "K": (8, "M1.5 1V11M7 1L1.5 7.5M3.6 5.2L7 11"),
    "L": (8, "M1.5 1V10.5H7"),
    "M": (9, "M1 11V1.5L4.5 7L8 1.5V11"),
    "N": (8, "M1.5 11V1L6.5 11V1"),
    "O": (8, "M3 1.5h2a2 2 0 0 1 2 2v5a2 2 0 0 1-2 2H3a2 2 0 0 1-2-2v-5a2 2 0 0 1 2-2Z"),
    "P": (8, "M1.5 11V1.5H5a2.5 2.5 0 0 1 0 5H1.5"),
    "Q": (8, "M3 1.5h2a2 2 0 0 1 2 2v5a2 2 0 0 1-2 2H3a2 2 0 0 1-2-2v-5a2 2 0 0 1 2-2ZM4.8 8.2L7.5 11.5"),
    "R": (8, "M1.5 11V1.5H5a2.5 2.5 0 0 1 0 5H1.5M4.5 6.5L7 11"),
    "S": (8, "M7 1.5H3a2 2 0 0 0-2 2v0.5a2 2 0 0 0 2 2h2a2 2 0 0 1 2 2v0.5a2 2 0 0 1-2 2H1"),
    "T": (8, "M0.5 1.5H7.5M4 1.5V11"),
    "U": (8, "M1.25 1V8.5a2 2 0 0 0 2 2h1.5a2 2 0 0 0 2-2V1"),
    "V": (8, "M1 1L4 11L7 1"),
    "X": (8, "M1 1L7 11M7 1L1 11"),
}
GLYPH_GAP = 2

PAGE_FILL = "#FFFFFF"
PAGE_STROKE = "#7D8FAD"
FOLD_FILL = "#DCE5F2"
# Colors of the application icon (src/images/icon-*.svg).
GRID = "#075BE8"
ARROW = "#17E0E5"
ARROW_OUTLINE = "#0044C8"


def label_paths(label, color):
    widths = [GLYPHS[c][0] for c in label]
    x = 32 - (sum(widths) + GLYPH_GAP * (len(label) - 1)) / 2
    parts = []
    for c, w in zip(label, widths):
        parts.append(f'<path transform="translate({x:g} 43)" d="{GLYPHS[c][1]}"/>')
        x += w + GLYPH_GAP
    return (
        f'  <g fill="none" stroke="{color}" stroke-width="2.2" stroke-linejoin="round">\n    '
        + "\n    ".join(parts)
        + "\n  </g>\n"
    )


def large_svg(label, band, ink):
    return (
        '<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64" viewBox="0 0 64 64">\n'
        f'  <path d="M15 3H41L53 15V59a2 2 0 0 1-2 2H15a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2Z" fill="{PAGE_FILL}" stroke="{PAGE_STROKE}" stroke-width="2"/>\n'
        f'  <path d="M41 3V13a2 2 0 0 0 2 2H53" fill="{FOLD_FILL}" stroke="{PAGE_STROKE}" stroke-width="2" stroke-linejoin="round"/>\n'
        f'  <rect x="18" y="19" width="30" height="16" rx="2" fill="{GRID}"/>\n'
        f'  <rect x="27" y="19" width="2" height="16" fill="{PAGE_FILL}"/>\n'
        f'  <rect x="37" y="19" width="2" height="16" fill="{PAGE_FILL}"/>\n'
        f'  <rect x="18" y="26" width="30" height="2" fill="{PAGE_FILL}"/>\n'
        f'  <path d="M9 24.5h17v-4.5l10 7-10 7v-4.5H9Z" fill="{ARROW}" stroke="{ARROW_OUTLINE}" stroke-width="1.5" stroke-linejoin="round"/>\n'
        f'  <rect x="8" y="39" width="48" height="20" rx="2" fill="{band}"/>\n'
        + label_paths(label, ink)
        + "</svg>\n"
    )


def small_svg(band):
    return (
        '<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16">\n'
        f'  <path d="M2.5 0.5H10.5L13.5 3.5V15.5H2.5Z" fill="{PAGE_FILL}" stroke="{PAGE_STROKE}"/>\n'
        f'  <path d="M10.5 0.5V3.5H13.5" fill="{FOLD_FILL}" stroke="{PAGE_STROKE}"/>\n'
        f'  <rect x="4" y="3" width="8" height="5" fill="{GRID}"/>\n'
        f'  <rect x="6" y="3" width="1" height="5" fill="{PAGE_FILL}"/>\n'
        f'  <rect x="9" y="3" width="1" height="5" fill="{PAGE_FILL}"/>\n'
        f'  <rect x="4" y="5" width="8" height="1" fill="{PAGE_FILL}"/>\n'
        f'  <rect x="1" y="10" width="14" height="5" fill="{band}"/>\n'
        "</svg>\n"
    )


def write_ico(path, images):
    """Writes an .ico with PNG-compressed entries (Windows Vista and later)."""
    header = struct.pack("<HHH", 0, 1, len(images))
    offset = len(header) + 16 * len(images)
    entries = b""
    data = b""
    for size, png in images:
        dim = 0 if size >= 256 else size
        entries += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(png), offset + len(data))
        data += png
    path.write_bytes(header + entries + data)


def write_icns(path, images):
    """Writes an .icns whose elements are all PNG data."""
    body = b"".join(t + struct.pack(">I", 8 + len(png)) + png for t, png in images)
    path.write_bytes(b"icns" + struct.pack(">I", 8 + len(body)) + body)


def main():
    for name, label, band, ink in FORMATS:
        large = large_svg(label, band, ink)
        small = small_svg(band)
        (HERE / f"{name}.svg").write_text(large, encoding="utf-8", newline="\n")
        (HERE / f"{name}-16.svg").write_text(small, encoding="utf-8", newline="\n")
        rendered = {}

        def render(svg, size):
            key = (svg is small, size)
            if key not in rendered:
                rendered[key] = resvg_py.svg_to_bytes(svg_string=svg, width=size, height=size)
            return rendered[key]

        images = [(s, render(large, s)) for s in LARGE_SIZES]
        images += [(s, render(small, s)) for s in SMALL_SIZES]
        write_ico(HERE / f"{name}.ico", images)
        icns = [(b"icp4", render(small, 16))] + [(t, render(large, s)) for t, s in ICNS_TYPES]
        write_icns(HERE / f"{name}.icns", icns)


if __name__ == "__main__":
    main()
