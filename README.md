<p align="center">
  <img src="src/images/icon.png" alt="TableSkim icon" width="128" height="128">
</p>

<h1 align="center">TableSkim</h1>

<p align="center">
  <img src="docs/images/gui-main-window.png" alt="TableSkim GUI showing a CSV file" width="760">
</p>

<p align="center">
  <img src="docs/images/tui-main.png" alt="TableSkim terminal UI" width="560">
</p>

TableSkim is a lightweight, fast viewer for tabular files. Open a large CSV,
an Excel workbook, a Parquet or JSON file or a DuckDB / SQLite database and start scrolling immediately: rows are loaded in the
background while the window stays responsive.

## Features

- **CSV / TSV** and **Excel (`.xlsx`, `.xlsm`)** files, including workbooks
  with multiple sheets
- CSV / TSV compressed with gzip, bzip2, xz or zstd (e.g. `data.csv.gz`)
  are decompressed on the fly
- CSV / TSV in UTF-8 (with or without a BOM), UTF-16 (with or without a
  BOM) and Shift_JIS (CP932, as written by Japanese Excel); the encoding is
  detected and can be changed from **View → Text Encoding**
- **Parquet** files and **DuckDB** / **SQLite** databases (every table and
  view is shown as a sheet); sorting and filtering run inside the database,
  so they work on tables of any size
- **JSON** files: an array of objects or one object per line (NDJSON /
  JSON Lines, `.ndjson`, `.jsonl`), also gzip- or zstd-compressed; object
  keys become columns, and nested objects and arrays are shown as text
- SQL on DuckDB, SQLite, Parquet and JSON files (**Edit → Run SQL...**, `:` in
  the terminal UI); a query is shown as a sheet. SQL that was run is kept
  in a history shared by all windows and sessions (**History**,
  `Ctrl+Up` / `Ctrl+Down`; `Up` / `Down`), and `Tab` completes table and
  column names and keywords
- Filters on any number of columns (**Edit → Filter...**, `f` in the
  terminal UI) that compare by type: numbers numerically, dates by day or
  time (`= 2024-01-31` matches any time that day, also in date text), and
  booleans as TRUE / FALSE; `matches` takes a regular expression
- Column statistics (**Edit → Column Statistics** or right-click a
  column; `S` in the terminal UI): row, empty and distinct counts,
  min / max, sum, mean and standard deviation, and the most frequent
  values, over the filtered rows; double-click a value to filter by it
- Histograms (**Edit → Histogram...** or right-click a column; `B` in the
  terminal UI) of numbers and dates in bins of round widths (whole days,
  months, years, ...), or of the most frequent values of other columns, in
  a window of their own; double-click a bar (`Enter`) to filter the sheet
  to its rows, and the histogram follows
- Scatter plots and 2D density of two columns (**Edit → Scatter Plot...**;
  the density with `M` in the terminal UI), with the correlation
  coefficient and the least-squares line
- Group By (**Edit → Group By...**; `A` in the terminal UI): count the rows
  by the values of some columns, with the sum, mean, min, max, ... of
  others, into a sheet of its own
- Column type marks in the headers (`#` numbers, `@` dates, `?` booleans)
  and per-column display formats: thousands separators, decimal places and
  date formats (right-click a column header → **Format**; `%`)
- Follow a growing file like `tail -f` (**File → Follow End of File**; `T`):
  appended lines of CSV / TSV files are read on their own, and the view
  stays at the last row; **File → Open Recent** lists the last files opened
- Go to a row, a cell (`B12` or `R12C2`) or a column by its name or
  letter (**Edit → Go To...**, `Ctrl+G` in both UIs)
- Hide columns (right-click a column header; `x` in the terminal UI) and
  show them all again (**View → Show All Columns**; `X`)
- Freeze the first columns so that they stay in place when scrolling
  sideways (right-click a column header or **View → Freeze Columns**; `|`)
- Edit cells of CSV / TSV and Excel files in edit mode (**Edit → Edit
  Mode**; `I` in the terminal UI), with undo, paste of tab-separated text
  and saving that rewrites only the edited lines or cells, keeping the
  encoding, compression and everything else of the file; nothing can change
  a file outside edit mode
- Export a sheet as shown (sorted, filtered, without hidden columns) to
  CSV, TSV, Excel (`.xlsx`) or Parquet (**File → Export View...**; `w`);
  Excel and Parquet keep numbers, booleans and dates typed, also when they
  come from CSV text
- Asynchronous loading on a worker thread, so large files open instantly
- Sorting and filtering CSV / TSV and Excel files of any size, in memory;
  when the rows read from a CSV / TSV file take about 100 MB, 500 MB and
  1 GB of memory, and every 2 GB from then on, TableSkim asks whether to
  keep reading or to show only the rows read so far
- Ragged rows, dates, merged cells and non-ASCII (e.g. Japanese) text and
  file names
- Option to use the first row of CSV / TSV and Excel files as a header
  (Parquet and databases have their own column names), monospace font, and
  several FLTK themes
- A user manual in English and Japanese, built in (**Help → User Manual**,
  `F1`) and installed as HTML for web browsers; see also [docs/](docs/README.md)
- A terminal UI with the same features, for use over SSH, started by the
  same `tableskim` command
- Native builds for Linux, macOS and Windows with all dependencies linked
  statically

## Usage

See the [user manual](docs/en/README.md) ([日本語](docs/ja/README.md)) for a
full guide with screenshots.

```sh
tableskim [options] [files...]
```

`tableskim` opens the GUI when a graphical display is available and the
terminal UI otherwise: on Linux when neither `DISPLAY` nor `WAYLAND_DISPLAY`
is set, and on macOS in an SSH session. Each file opens in its own window.
The terminal UI shows exactly one file, which must be given on the command
line (it exits with an error otherwise).

| Option            | Description                                               |
| ----------------- | --------------------------------------------------------- |
| `-t`, `--tui`     | Use the terminal UI even if a graphical display is available |
| `--log <file>`    | Write log messages to a file                              |
| `--encoding <name>` | Encoding of CSV / TSV files: `auto` (default), `utf-8`, `utf-16le`, `utf-16be` or `cp932` (`shift_jis`) |
| `-v`, `--verbose` | Enable verbose (debug) logging                            |
| `-V`, `--version` | Show version information and exit                         |

Files can also be opened from **File → Open...** (`Ctrl+O`).

On macOS the command is the executable inside the app bundle; to put it on
the `PATH`:

```sh
ln -s /Applications/TableSkim.app/Contents/MacOS/TableSkim /usr/local/bin/tableskim
```

On Windows, `TableSkim.exe` is a GUI application (without a console) for the GUI.
Use `tableskim-tui.exe`, installed next to it, for the terminal UI: it is a
console application, so it runs in the terminal it is started from.

### Terminal UI

Log messages are written only with `--log <file>` (they would otherwise be
drawn over the screen). Press `?` for the key list. Main keys: arrows or
`hjkl` to move, `Ctrl+G` to go to a row, cell or column, `Shift`+arrows or
`v` to select, `x` / `X` to hide the column / show all columns, `|` to
freeze the columns up to the cursor, `Enter` to show a whole cell in the
sidebar, `S` for the statistics of the cursor's column, `B` for its
histogram, `y` to copy the
selection as TSV, `w` to export the sheet as shown, `Tab` to switch sheets, `s` for the sidebar, `H` to use
the first row as header (CSV / TSV and Excel), `E` to switch the text
encoding, `r` to revert, `q` to quit.

Copying uses the OSC 52 escape sequence, so it works over SSH; the terminal
must allow it (inside tmux, enable `set-clipboard`).

## Building

Requirements:

- A C++20 compiler (GCC, Clang, AppleClang or MSVC 2026)
- CMake 3.22 or newer (Ninja recommended)
- On Linux: fontconfig development files

All other dependencies (FLTK, DuckDB, SQLite, OpenXLSX, csv-parser, CLI11,
spdlog, zlib, bzip2, xz, zstd, RE2, Abseil, Catch2, FTXUI) are downloaded with CMake's
`FetchContent` during the first configure. The first build compiles DuckDB
and therefore takes a while.

### Linux / macOS

```sh
./build.sh            # release build + tests
./build.sh debug      # debug build + tests
```

The build output goes to `build/<preset>/`. The steps can also be run
manually:

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
```

To build only the terminal UI (no FLTK, X11 or fontconfig needed, e.g. for
a headless server), configure with `-DTABLESKIM_BUILD_GUI=OFF`; `tableskim`
then always starts the terminal UI.

On Linux, system libraries can be used instead of the bundled ones:

| CMake option                            | Uses system copy of         |
| --------------------------------------- | --------------------------- |
| `-DTABLESKIM_USE_SYSTEM_FLTK=ON`        | FLTK (1.4 or newer)         |
| `-DTABLESKIM_USE_SYSTEM_SQLITE=ON`      | SQLite                      |
| `-DTABLESKIM_USE_SYSTEM_COMPRESSION=ON` | zlib, bzip2, liblzma, zstd  |

### Windows

Use the Visual Studio 2026 presets:

```bat
cmake --preset vs-release
cmake --build --preset vs-release
```

or, from a Developer Command Prompt, the Ninja presets (which also use
ccache when it is installed):

```bat
cmake --preset msvc-release
cmake --build --preset msvc-release
```

## Packaging

```sh
./package.sh [deb|rpm|dmg|zip|all]     # Linux / macOS
package.bat  [exe|zip|msix|all]        # Windows
```

Packages are written to the release build directory. Pushing a tag such as
`v0.1.0` builds packages for every platform with GitHub Actions.

## Development

- `src/common/` — core library: data model, file backends, threading
- `src/gui/` — FLTK GUI (layout in `gui.fl`)
- `src/tui/` — FTXUI terminal UI
- `src/main.cxx` — the `tableskim` entry point, which picks the GUI or TUI
- `tests/` — Catch2 unit tests
- `demo/` — script that generates the sample files in `demo/data/`

See [AGENTS.md](AGENTS.md) for architecture notes, coding conventions and
instructions for adding a new file format.

## License

TableSkim is licensed under the [GNU Affero General Public License v3.0](LICENSE).
The bundled fonts, BIZ UDPGothic and M PLUS 1 Code, are distributed under
the SIL Open Font License 1.1 ([src/fonts/](src/fonts/)). The license texts
of the fonts and of all bundled libraries are shown in **Help → About...**
(at the end of the `?` help in the terminal UI) and installed as
`THIRD_PARTY_LICENSES.txt`.
