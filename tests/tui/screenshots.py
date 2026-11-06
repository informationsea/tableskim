#!/usr/bin/env python3
"""Takes the manual's screenshots of the terminal UI (docs/images/tui-*.png).

Runs the TUI in a pseudo-terminal as the end-to-end tests do (tui_harness),
then draws pyte's screen as a dark terminal would, with DejaVu Sans Mono
(and IPAGothic for wide characters). Needs pyte and Pillow:

    python3 tests/tui/screenshots.py --tableskim build/debug/tableskim \\
        [--examples demo/examples] [--out docs/images] [names ...]

Look at each image before committing it.
"""

import argparse
import os
import shutil
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
WIDE_FONT = next((p for p in [
    "/usr/share/fonts/opentype/ipafont-gothic/ipag.ttf",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
] if os.path.exists(p)), FONT)

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

    iris = [W("150 rows")]
    return {
        "tui-main.png": ("iris.csv", iris),
        "tui-help.png": ("iris.csv", iris + [keys("?"), W("Esc: close")]),
        "tui-search.png": ("iris.csv", iris + [keys("/", "setosa", "enter"), W("1/50")]),
        "tui-stats.png": ("iris.csv", iris + [keys("l", "l", "S"), W("Mean")]),
        "tui-histogram.png": ("iris.csv", iris + [keys("l", "l", "B"), W("150 numbers in 30 bins"),
            keys("down", "down"), W("[1.4, 1.6)")]),
        "tui-histogram-dates.png": ("hour.csv.xz", [W("dteday"), keys("l", "B"), W("17379 dates")]),
        "tui-histogram-stacked.png": ("iris.csv", iris + [keys("l", "l", "B"), W("150 numbers"),
            keys("g", "g", "g", "g"), W("by species"), keys("down", "down", "tab"), W("setosa")]),
        "tui-density.png": ("hour.csv.xz", [W("dteday"), keys(*["l"] * 10, "v", *["l"] * 6, "M"),
            W("17379 rows plotted")]),
        "tui-density-categories.png": ("penguins.csv", [W("species"), keys("v", *["l"] * 5, "M"),
            W("Box plot: species × body_mass_g"), keys("b"), W("Density: species × body_mass_g")]),
        "tui-boxplot.png": ("penguins.csv", [W("species"), keys("v", *["l"] * 5, "M"),
            W("Box plot: species × body_mass_g")]),
        "tui-density-colored.png": ("iris.csv", iris + [keys("l", "l", "v", "l", "M"),
            W("Density: petal_length × petal_width"), keys(*["c"] * 3), W("by species")]),
        "tui-filter.png": ("iris.csv", iris + [keys(*["l"] * 4, "=", "f"), W("setosa")]),
        "tui-filtered.png": ("iris.csv", iris + [keys(*["l"] * 4, "="), W("50 rows of 150")]),
        "tui-group-by.png": ("iris.csv", iris + [keys(*["l"] * 4, "A"), W("Enter: group")]),
        "tui-group-by-result.png": ("iris.csv", iris + [keys(*["l"] * 4, "A"), W("Enter: group"),
            keys("enter"), W("3 groups"), keys("l", "o", "o"), W("50")]),
        "tui-format.png": ("iris.csv", iris + [keys("l", "l", "%"), W("Decimal places")]),
        "tui-sql-prompt.png": ("iris.parquet", iris + [keys(":"), W("Enter: run"),
            keys("SELECT * FROM iris")]),
        "tui-sql-result.png": ("iris.parquet", iris + [keys(":"), W("Enter: run"),
            keys("SELECT species, count(*) AS flowers, round(avg(petal_length), 3) AS mean_petal_length FROM iris GROUP BY species ORDER BY species", "enter"),
            W("3 rows")]),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--tableskim", required=True)
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
    parser.add_argument("--examples", default=os.path.join(root, "demo", "examples"))
    parser.add_argument("--out", default=os.path.join(root, "docs", "images"))
    parser.add_argument("names", nargs="*")
    args = parser.parse_args()
    for name, (file, steps) in shots().items():
        if args.names and name not in args.names:
            continue
        with tempfile.TemporaryDirectory() as home:
            source = os.path.join(args.examples, file)
            if file == "iris.parquet":
                # Convert a copy, as the manual explains, without adding files to examples/.
                csv_path = os.path.join(home, "iris.csv")
                shutil.copyfile(os.path.join(args.examples, "iris.csv"), csv_path)
                with Tui(os.path.abspath(args.tableskim), csv_path, home=home) as convert:
                    convert.wait_text("150 rows")
                    convert.send("P")
                    convert.wait_text("iris.parquet")
                    convert.wait_text("150 rows")
                source = os.path.join(home, "iris.parquet")
            with Tui(os.path.abspath(args.tableskim), source, home=home,
                     columns=COLUMNS, lines=LINES, env={"COLORTERM": "truecolor", "NO_COLOR": ""}, screen=DimScreen) as tui:
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
