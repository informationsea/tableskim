#!/usr/bin/env python3
"""Takes the manual's screenshots of the terminal UI (docs/images/tui-*.png).

Runs the TUI in a pseudo-terminal as the end-to-end tests do (tui_harness),
then draws pyte's screen as a dark terminal would, with DejaVu Sans Mono
(and IPAGothic for wide characters). Needs pyte and Pillow:

    python3 tests/tui/screenshots.py --tableskim build/debug/tableskim \\
        [--demo-data demo/data] [--out docs/images] [names ...]

Look at each image before committing it.
"""

import argparse
import os
import sys
import tempfile
import time
import unicodedata

import pyte
from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tui_harness import Tui  # noqa: E402

COLUMNS = 98
LINES = 30
CELL_W, CELL_H = 10, 20
PAD_X, PAD_Y = 8, 10
FONT_SIZE = 16
FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
BOLD_FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf"
WIDE_FONT = "/usr/share/fonts/opentype/ipafont-gothic/ipag.ttf"

BACKGROUND = (0x1e, 0x1e, 0x1e)
FOREGROUND = (0xd4, 0xd4, 0xd4)
DIM = (0x8a, 0x8a, 0x8a)
# The 16 colours, as VS Code's terminal draws them.
PALETTE = {
    "black": (0x00, 0x00, 0x00), "red": (0xcd, 0x31, 0x31), "green": (0x0d, 0xbc, 0x79),
    "brown": (0xe5, 0xe5, 0x10), "yellow": (0xe5, 0xe5, 0x10), "blue": (0x24, 0x72, 0xc8),
    "magenta": (0xbc, 0x3f, 0xbc), "cyan": (0x11, 0xa8, 0xcd), "white": (0xe5, 0xe5, 0xe5),
    "brightblack": (0x66, 0x66, 0x66), "brightred": (0xf1, 0x4c, 0x4c), "brightgreen": (0x23, 0xd1, 0x8b),
    "brightbrown": (0xf5, 0xf5, 0x43), "brightyellow": (0xf5, 0xf5, 0x43), "brightblue": (0x3b, 0x8e, 0xea),
    "brightmagenta": (0xd6, 0x70, 0xd6), "brightcyan": (0x29, 0xb8, 0xdb), "brightwhite": (0xff, 0xff, 0xff),
}


class DimScreen(pyte.Screen):
    """A pyte screen that keeps faint text (SGR 2) in the unused `blink` attribute."""

    def select_graphic_rendition(self, *attrs):
        super().select_graphic_rendition(*attrs)
        dim = self.cursor.attrs.blink
        values = list(attrs) or [0]
        i = 0
        while i < len(values):
            value = values[i]
            if value in (38, 48):  # a colour's parameters follow
                i += 5 if i + 1 < len(values) and values[i + 1] == 2 else 3
                continue
            if value in (0, 22):
                dim = False
            elif value == 2:
                dim = True
            i += 1
        self.cursor.attrs = self.cursor.attrs._replace(blink=dim)


def colour(name, default):
    if name == "default":
        return default
    if name in PALETTE:
        return PALETTE[name]
    if len(name) == 6:
        return tuple(int(name[k:k + 2], 16) for k in (0, 2, 4))
    return default


# Box drawing: which sides of the cell a character's lines reach (up,
# right, down, left), and whether they are heavy.
LINES_OF = {
    "│": "u.d.", "─": ".r.l", "┌": ".rd.", "┐": "..dl", "└": "ur..", "┘": "u..l",
    "╭": ".rd.", "╮": "..dl", "╰": "ur..", "╯": "u..l", "├": "urd.", "┤": "u.dl",
    "┬": ".rdl", "┴": "ur.l", "┼": "urdl", "┃": "U.D.", "━": ".R.L",
}
# Left blocks: the eighths of the cell they fill.
EIGHTHS = {"▏": 1, "▎": 2, "▍": 3, "▌": 4, "▋": 5, "▊": 6, "▉": 7, "█": 8}


def draw_shape(draw, char, left, top, fg):
    """Draws a block or box-drawing character as shapes, so that
    neighbouring cells join; False for other characters."""
    if char in EIGHTHS:
        # A pixel short at the top and the bottom, as a font's glyph is:
        # bars on neighbouring lines stay apart, and an inverted one shows.
        draw.rectangle([left, top + 1, left + CELL_W * EIGHTHS[char] // 8 - 1, top + CELL_H - 2], fill=fg)
        return True
    sides = LINES_OF.get(char)
    if not sides:
        return False
    cx, cy = left + CELL_W // 2, top + CELL_H // 2
    for side in sides:
        if side == ".":
            continue
        width = 2 if side.isupper() else 1
        x0, y0, x1, y1 = {
            "u": (cx, top, cx, cy), "d": (cx, cy, cx, top + CELL_H - 1),
            "l": (left, cy, cx, cy), "r": (cx, cy, left + CELL_W - 1, cy),
        }[side.lower()]
        draw.line([x0, y0, x1, y1], fill=fg, width=width)
    return True


def render(screen, path):
    fonts = {
        False: ImageFont.truetype(FONT, FONT_SIZE),
        True: ImageFont.truetype(BOLD_FONT, FONT_SIZE),
    }
    wide = ImageFont.truetype(WIDE_FONT, FONT_SIZE)
    image = Image.new("RGB", (COLUMNS * CELL_W + 2 * PAD_X, LINES * CELL_H + 2 * PAD_Y), BACKGROUND)
    draw = ImageDraw.Draw(image)
    for y in range(LINES):
        line = screen.buffer[y]
        for x in range(COLUMNS):
            char = line[x]
            fg = colour(char.fg, DIM if char.blink else FOREGROUND)
            bg = colour(char.bg, BACKGROUND)
            if char.reverse:
                fg, bg = (bg if char.bg != "default" else BACKGROUND), (fg if char.fg != "default" else FOREGROUND)
            left, top = PAD_X + x * CELL_W, PAD_Y + y * CELL_H
            span = 2 if char.data and unicodedata.east_asian_width(char.data[0]) in "WF" else 1
            if bg != BACKGROUND:
                draw.rectangle([left, top, left + CELL_W - 1, top + CELL_H - 1], fill=bg)
            if not char.data or char.data == " ":
                continue
            if draw_shape(draw, char.data, left, top, fg):
                continue
            font = wide if span == 2 else fonts[bool(char.bold)]
            draw.text((left, top + 1), char.data, font=font, fill=fg)
    image.save(path)


def shots():
    """The images: name -> (file, steps), a step being text to wait for or keys to send."""
    W = str  # wait for this text

    def keys(*names):
        return list(names)

    return {
        # Salary, the 600000 - 650000 bar selected.
        "tui-histogram.png": ("normal.csv", [
            W("54 rows"), keys(*["l"] * 6, "B"), W("53 numbers in 30 bins"),
            keys(*["down"] * 12), W("[600000, 650000)"),
        ]),
        "tui-histogram-dates.png": ("normal.csv", [
            W("54 rows"), keys(*["l"] * 7, "B"), W("53 dates in 23 bins"),
        ]),
        # Age stacked by City, the Sendai part of the 34 - 36 bar selected.
        "tui-histogram-stacked.png": ("normal.csv", [
            W("54 rows"), keys("l", "l", "l", "B"), W("53 numbers"),
            keys("g", "g", "g", "g"), W("by City"), W("Fukuoka"),
            keys(*["down"] * 7), W("[34, 36)"),
            keys("tab", "tab", "tab", "tab"), W("Sendai: 3 rows"),
        ]),
        "tui-density.png": ("large.parquet", [
            W("10000 rows"), keys("l", "l", "l", "l", "M"), W("10000 rows plotted"),
        ]),
        "tui-density-categories.png": ("large.parquet", [
            W("10000 rows"), keys("l", "l", "v", "l", "l", "M"), W("Box plot: Category × Score"),
            keys("b"), W("Density: Category × Score"),
        ]),
        # Age and Salary coloured by City.
        "tui-density-colored.png": ("normal.csv", [
            W("54 rows"), keys("l", "l", "l", "v", "l", "l", "l", "M"), W("Density: Age × Salary"),
            keys("c", "c", "c", "c"), W("by City"), W("Fukuoka"),
        ]),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--tableskim", required=True)
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
    parser.add_argument("--demo-data", default=os.path.join(root, "demo", "data"))
    parser.add_argument("--out", default=os.path.join(root, "docs", "images"))
    parser.add_argument("names", nargs="*")
    args = parser.parse_args()
    for name, (file, steps) in shots().items():
        if args.names and name not in args.names:
            continue
        with tempfile.TemporaryDirectory() as home:
            with Tui(os.path.abspath(args.tableskim), os.path.join(args.demo_data, file), home=home,
                     columns=COLUMNS, lines=LINES, env={"COLORTERM": "truecolor"}, screen=DimScreen) as tui:
                for step in steps:
                    if isinstance(step, str):
                        tui.wait_text(step)
                    else:
                        tui.send(*step)
                # The last frames: drawn once the program is idle.
                time.sleep(0.5)
                tui.pump(0.5)
                render(tui.screen, os.path.join(args.out, name))
        print(name)


if __name__ == "__main__":
    main()
