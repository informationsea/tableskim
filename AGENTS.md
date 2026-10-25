# AGENTS.md

Guidance for coding agents working on TableSkim.

## Project overview

TableSkim is a lightweight viewer for tabular files (CSV/TSV/VCF, optionally
gzip/bzip2/xz/zstd compressed, Excel, DuckDB, SQLite, Parquet and JSON / NDJSON). It is written in C++20 and has two
frontends sharing one core library, linked into a single executable:

| Target             | Sources                            | Notes                                                    |
| ------------------ | ---------------------------------- | -------------------------------------------------------- |
| `tableskim-common` | `src/common/`                      | Static library: data model, backends, threading          |
| `tableskim-gui`    | `src/gui/`                         | Static library: the FLTK GUI (`runGui()`), with the GUI only |
| `tableskim`        | `src/main.cxx`, `src/tui/`         | FLTK GUI (links `tableskim-gui`) and FTXUI TUI (`tableskim`; `TableSkim` app bundle / `.exe` on macOS and Windows) |
| `tableskim-tui`    | `src/main.cxx`, `src/tui/`         | Windows with the GUI only: TUI-only console executable (`tableskim-tui.exe`) |
| `tableskim-tests`  | `tests/`                           | Catch2 v3 unit tests                                     |
| `tableskim-gui-tests` | `tests/gui/`                    | Linux with the GUI only: Catch2 tests driving the GUI under Xvfb |

`main()` parses the shared options (`cli_options.{hxx,cxx}` in
`src/common/`) and calls `runGui()` or `runTui()`: the TUI with `-t/--tui`
or when `graphicalDisplayAvailable()` is false (Linux: no `DISPLAY` /
`WAYLAND_DISPLAY`; macOS: SSH session; Windows: never, since `TableSkim.exe`
is a GUI-subsystem program; the TUI is the separate `tableskim-tui.exe`, built
from the same `main.cxx` without `TABLESKIM_HAS_GUI`). Add new options
there so both frontends get them.

All third-party dependencies (FLTK, DuckDB, SQLite, OpenXLSX, csv-parser,
Catch2, FTXUI, CLI11, spdlog, zlib, bzip2, xz, zstd, RE2, Abseil) are pulled with `FetchContent` in
`cmake/Dependencies.cmake` and linked statically (OpenXLSX is patched at
every configure by `cmake/patches/openxlsx.cmake`, which must leave patched
files alone). There is no system package
requirement except a compiler, CMake ≥ 3.22, and (on Linux) fontconfig.
On Linux, FLTK can optionally come from the system (FLTK ≥ 1.4 with its CMake
config package) by configuring with `-DTABLESKIM_USE_SYSTEM_FLTK=ON`.
Likewise `-DTABLESKIM_USE_SYSTEM_SQLITE=ON` and
`-DTABLESKIM_USE_SYSTEM_COMPRESSION=ON` (zlib, bzip2, liblzma, zstd) use the
system copies; each option is independent.
`-DTABLESKIM_X11_ONLY=ON` builds the bundled FLTK without Wayland (still
drawing with Cairo and Pango); the release workflow uses it for the ZIP
built on Rocky Linux 8 for older distributions (glibc 2.28).
`-DTABLESKIM_BUILD_GUI=OFF` drops the GUI (and FLTK) entirely: `src/gui/`
is not compiled, `TABLESKIM_HAS_GUI` is left undefined and `main()` always
runs the TUI. Keep GUI-only code out of `src/common/` and `src/tui/`.

## Build and test

```sh
./build.sh debug        # or: ./build.sh release (default)
```

`build.sh` configures with the presets in `CMakePresets.json` (Ninja if
available, otherwise Unix Makefiles) into `build/<preset>/`, builds, and then
runs the tests. The individual steps are:

```sh
cmake --preset debug                 # first time only
cmake --build --preset debug
ctest --preset debug                 # output is shown on failure
# or run a subset:
./build/debug/tableskim-tests "normal.csv*"
```

- The first configure clones and builds DuckDB and the other dependencies; it
  takes a long time. Reuse the existing `build/` directory instead of wiping it.
- If `ccache` is installed, CMake compiles through it (this project and the
  dependencies; Ninja and Makefile generators only, `-DTABLESKIM_USE_CCACHE=OFF`
  disables it). CI and the release workflow keep its cache (`.ccache`) with
  `actions/cache`; the release builds read the cache CI saved for the same
  runner (`ccache-<runner>-*`), and only the GUI builds save one, the
  GUI-less release builds just read it. Configure again after installing it.
- Dependencies are always built as Release, even in Debug builds (except with
  MSVC, where runtimes cannot be mixed). Keep this in mind when editing
  `cmake/Dependencies.cmake`.
- Git dependencies are cloned shallow at their tag (`GIT_SHALLOW`), and
  archives carry a `URL_HASH`: update the hash with the URL. DuckDB gets its
  version from `OVERRIDE_GIT_DESCRIBE`, so that it does not depend on the
  shallow clone having fetched the tag.
- Debug builds define `TABLESKIM_DEBUG=1`, which enables debug logging.
- Windows uses the `vs-debug` / `vs-release` presets (Visual Studio 2026, MSVC)
  or, from a Developer Command Prompt, `msvc-debug` / `msvc-release` (Ninja,
  MSVC). CI and the release workflow use `msvc-release` so that ccache works
  (Visual Studio generators ignore compiler launchers); `package.bat` follows
  `TABLESKIM_PRESET`.

### Demo / test data

`demo/data/` contains generated sample files (normal, ragged, large, dates,
multilingual, multi-sheet, merged cells) in CSV, XLSX, Parquet, SQLite and
DuckDB formats, plus compressed CSV/TSV (`normal.csv.{gz,bz2,xz,zst}`,
`large.tsv.gz`) and CSV/TSV in other encodings (`normal-cp932.csv`,
`normal-utf16le-bom.tsv`, ..., `cp932-sample.csv`) and JSON (`normal.json`,
`normal.ndjson`, `normal.jsonl.gz`, `nested.json`). Tests read them via the `TABLESKIM_DEMO_DATA_DIR` compile
definition. To regenerate:

```sh
cd demo
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
.venv/bin/python create-demo-data.py
```

Do not edit the files in `demo/data/` by hand; change `create-demo-data.py`
and regenerate.

`super-large.csv.xz`, `super-large.xlsx` and `super-large.parquet` (zstd,
1,000,000 rows) are generated
too but are not committed (see `demo/.gitignore`); `--no-super-large` skips
them. Their tests are hidden and run only on request:

```sh
./build/debug/tableskim-tests "[.super-large]"
```

### Packaging

`./package.sh [deb|rpm|dmg|zip|all]` (Linux/macOS) and
`package.bat [exe|zip|msix|all]` (Windows) build a release and run CPack.
Packaging rules live in `cmake/Packaging.cmake` and `packaging/`. Linux
package names include the build distribution from `/etc/os-release`
(e.g. `tableskim_0.2.2_ubuntu24.04_amd64.deb`); `-DTABLESKIM_PACKAGE_OS_TAG=`
overrides it.

## Architecture

The core API is in `src/common/table.hxx` and `src/common/table_source.hxx`;
read their header comments before changing anything in `src/common/`.

- **Backends** implement `DataSource` (one file) and `SheetSource` (one
  sheet), and register a `BackendFactory` whose `probe()` scores a file by
  path and leading bytes. Built-in backends are registered in
  `table_registry.cxx` (`registerCsvBackend`, `registerExcelBackend`,
  `registerDuckDbBackend`, `registerParquetBackend`, `registerSqliteBackend`).
  To add a format: create `table_<format>.{hxx,cxx}`, implement the two
  interfaces, register it in `table_registry.cxx`, add the files to
  `tableskim-common` in `CMakeLists.txt`, and add `tests/table_<format>_tests.cxx`.
  Override `DataSource::usesHeaderOption()` to return true only if the
  format honours `OpenOptions::header` (CSV, Excel); both UIs disable the
  "first row as header" option otherwise (`TableBook::usesHeaderOption()`).
- The DuckDB backend (`table_duckdb.cxx`) pages with `LIMIT`/`OFFSET` and
  translates `ViewSpec` into SQL that mirrors the in-memory sort/filter
  semantics in `table.cxx` (`cellMatchesCondition()`: numbers, dates by
  `parseTemporal()`, booleans, RE2 regular expressions); keep the two in
  sync. `checkFilterMatches()` in `tests/table_duckdb_tests.cxx` compares
  them. The UIs check `filterError()` before applying a filter. Formats DuckDB reads
  natively reuse it through `openDuckDbRelations()` (see `table_parquet.cxx`
  and `table_json.cxx`). Their DuckDB extensions are linked statically:
  add new ones to `BUILD_EXTENSIONS` and the link lists in
  `cmake/Dependencies.cmake`, since autoloading is off.
- The SQLite backend (`table_sqlite.cxx`) uses the SQLite C API directly. Its
  filters call `cellMatchesCondition()` (the in-memory semantics) as an SQL
  function; a sort/filter is built once into a TEMP table of row positions.
- **`SourceCapabilities`** decides how the model treats a source:
  `random_access = false` means rows are read strictly in order (CSV, Excel);
  `query_pushdown = true` means sort/filter are delegated via `applyView()`.
  Without pushdown, sort/filter are done in memory over every row, which
  sequential sources keep cached once read (no eviction). For CSV/TSV
  (`TableBook` turns it off for other formats), reading pauses when the
  cached rows reach each of `Options::memory_checkpoints` (100 MiB,
  500 MiB, 1 GiB) and then each multiple of `memory_checkpoint_interval`
  (2 GiB), by `RowBlock::approxBytes()`: `loadBlock()` does nothing while
  `memoryCheckpoint()` is set, and the UIs ask whether to
  `continueReading()` or `stopReading()` (the table then ends at the rows
  read; `readingStopped()`).
- **`TableBook`** owns one `DataSource` and a single worker thread
  (`std::jthread`). **`TableModel`** (one per sheet) is the UI-facing object.
  Every public `TableModel` method is non-blocking: uncached cells come back
  as `CellState::Loading`, a request is queued to the worker, and a
  `TableEvent` is emitted once the data is ready.
- `SheetSource` / `DataSource` methods are only called on the worker thread
  (except `DataSource` construction), so backends need no locking.
- Listeners registered with `TableModel::subscribe()` run **on the worker
  thread**. UIs must hop back to their own loop (the GUI uses `Fl::awake`
  and `Fl::lock()`; see `GuiTableView::awakeHandler`). The returned
  `Subscription` must be destroyed before its target goes away.
- Cell values are `CellValue` (`std::variant` of NULL, string, int64,
  double, bool, `Date`, `DateTime`); rows are delivered in immutable,
  shared `RowBlock`s. Avoid per-cell heap allocations and virtual calls.
  Rows may be ragged (different lengths).
- The TUI (`TuiApp` in `tui_app.cxx`, one `TuiTableView` per sheet) shows
  the single file given on the command line; it has no open command. It hops
  back with `ftxui::App::Post` plus `PostEvent(Event::Custom)`, since a
  posted closure alone does not redraw. The grid is built in a custom node's
  `SetBox()`, when its size is known. Log output goes to `--log` or nowhere,
  as it would corrupt the screen.
- `TableBook::executeSql()` runs `DataSource::executeSql()` (DuckDB, SQLite
  and Parquet files, `supportsSql()`; a Parquet file is the view
  `DuckDbRelation::sql_name`) as a task on the book's worker. A single
  query becomes a TEMP view shown as an extra sheet ("SQL Result"), so
  `sheetNames()` can grow; running another query retires the old model
  (`TableModel::retire()`) and `sheet()` returns a new one. The view dies
  with the book, so the UIs run the last query again after a reopen
  (GUI: Edit > Run SQL..., `gui_sql.cxx`; TUI: `:`).
- The SQL editors share `SqlHistory` (`sql_history.{hxx,cxx}`): a file in
  `userDataDirectory()` (`file_util.hxx`), read again before every add so
  that windows and processes keep each other's entries, walked with
  `SqlHistoryCursor`. Tab completes with `completeSql()` (`sql_text.cxx`,
  lexical only: the word before the cursor, `name.` qualifiers and FROM /
  JOIN aliases) over `TableBook::loadSqlSchema()`, which runs
  `DataSource::sqlSchema()` on the worker; backends leave out their own
  `tableskim_*` objects. Tests must not use the default history path.
- `exportTextFile()` (`table_export.{hxx,cxx}`) converts a text file
  (`DataSource::isTextFormat()`) to Parquet or DuckDB by reading it through
  the normal backend, staging the cells as VARCHAR in DuckDB, inferring
  column types and writing with `COPY ... (FORMAT parquet)` or into an
  attached database. The GUI runs it behind a progress dialog
  (`gui_convert.cxx`), the TUI on a thread (`P` / `D`).
- `exportView()` (`table_export.{hxx,cxx}`) writes a sheet as shown (its
  applied sort and filter, minus the UI's hidden columns) to CSV, TSV,
  Excel or Parquet. It reads the rows through `TableModel::readView()`,
  which queues chunks of view rows on the worker a little ahead of the
  reader (random-access sources are read past the cache, in large blocks)
  and fails if the view changes meanwhile, so SQL results and in-memory
  views export too. Excel and Parquet are staged in DuckDB first, as the
  column types depend on every row; Excel is written by `XlsxWriter`
  (`xlsx_writer.{hxx,cxx}`), a streaming writer with inline strings and its
  own ZIP output on zlib, since OpenXLSX keeps everything in memory and
  looks up shared strings linearly. The GUI runs it behind the progress
  dialog of `gui_convert.cxx` (File > Export View...), the TUI on the
  conversion thread (`w`).
- `TableModel::startColumnStats()` computes `ColumnStats` (`column_stats.{hxx,cxx}`)
  for one column over the rows of the view, scanning a block per worker step
  like a search (`scanChunkLocked()`); a view change starts it over. The
  accumulator follows the in-memory semantics (`isEmptyCell()`,
  `compareCells()`, numeric text via `parseNumber()`). Pushdown sources may
  implement `SheetSource::columnValueCounts()` instead (DuckDB and Parquet:
  one `GROUP BY` over the filtered rows, most frequent first): they feed
  each distinct value, converted as `read()` does, with its count into the
  same accumulator, so the results match a scan by construction.
- `TableModel::startHistogram()` counts a column into bins
  (`histogram.{hxx,cxx}`) in two passes like the statistics: a
  `HistogramRange` finds the kind (numbers or times, as the filters read
  them) and the range, then `HistogramCounter` counts into the bins of its
  layout; pushdown sources answer each pass with `columnValueCounts()`.
  `histogramBinConditions()` (`cell_format`) gives a bin's filter, which
  must keep exactly the rows counted in it (tested). The GUI shows it in
  `HistogramWindow` (`gui_histogram.cxx`), the TUI instead of the sheet
  (`B`).
- `TableModel::startScatter()` plots two columns against each other
  (`scatter.{hxx,cxx}`) the same way: `ScatterRange` takes each column's
  range (pushdown sources by `columnValueCounts()` per column), then
  `ScatterCounter` scans the view, counting a density grid of two
  histograms' bins (`HistogramCounter::binOf()`, so that
  `densityCellConditions()` keeps exactly a cell's rows; tested) and
  keeping a reservoir sample of points with their view rows (fixed seed)
  and `ScatterStats` (Pearson's r and the least-squares line, by Welford's
  updates) over every row counted.
  The GUI shows points or the density in `ScatterWindow` (`gui_scatter.cxx`),
  the TUI the density only, instead of the sheet (`M`). A text X makes the
  plot categorical: its most frequent values (`x_axis.top`) are the
  categories, plus "(other)"; the counter keeps each category's Y values
  for `boxStats()` (box plots, both UIs).
- The GUI's histogram and scatter windows write their chart widget as PNG
  (`Fl_Image_Surface`, `fl_write_png()`) or SVG (`Fl_SVG_File_Surface`)
  with `writeWidgetImage()` (`gui_image_export.cxx`), so the image is what
  the widget draws.
- `groupByView()` (`group_by.{hxx,cxx}`) groups the rows of a view, read
  through `readView()` like an export, with a `GroupByAccumulator`: groups
  keyed by their values (sort order, exact values apart), and one
  `ColumnStatsAccumulator` per aggregated column, so the aggregates follow
  the statistics. The result (`ResultSheet`) becomes `ResultRows`
  (shared blocks the sheet's model caches as they are, so the rows are held
  once) and is shown with `TableBook::addResultSheet()`: a sheet backed by
  a `ResultSheetSource` (sequential, in memory) that a later result of the
  same name replaces, retiring its model like an SQL result. The UIs keep
  the `ResultRows` and add the sheet again after a reopen (GUI: Edit >
  Group By..., `gui_group_by.cxx`; TUI: `A`), both as `GROUP_BY_SHEET`.
- `TableModel::columnTypes()` judges what each column holds
  (`ColumnTypeCounter`, `column_type.{hxx,cxx}`) from the first
  `Options::type_sample_rows` rows read, counted in `loadBlock()`; the
  headers show `columnKindMark()`. Display formats (`ColumnFormat`,
  `ColumnFormats` in `cell_format`) belong to a view like hidden columns and
  only change `cellDisplayText()`; copies and exports keep the values.
- Follow (tail -f; GUI: File > Follow End of File, TUI: `T`) polls the
  file; `TableBook::readAppended()` asks each sheet's model to read the
  rows appended since the end (`TableModel::readAppendedRows()`, merging
  them into the partly filled last block under the lock, so that an edit
  made meanwhile is kept). Rows appended to an unsorted view join its end,
  so searches, statistics, histograms and scatter plots go on with them
  (`continueAfterAppendLocked()`; finished histogram and scatter jobs are
  kept for this, and start over only if a new value is outside their bins,
  `fits()`); a sorted view is applied again.
  Only sources with `DataSource::readsAppendedRows()` can (uncompressed
  CSV/TSV in UTF-8 or CP932: `SheetSource::readAppended()` parses the whole
  records after the byte offset counted while reading, after checking the
  bytes before it, finding record ends as csv-parser does: a quote opens a
  quoted field only at the start of a field). `AppendResult::Changed` makes
  the UIs reopen the file; they take the file's stamp as read only when
  `caught_up` (no row being written is left).
- Recent files (GUI: File > Open Recent) are kept by `RecentFiles`
  (`recent_files.{hxx,cxx}`) in `userDataDirectory()` like the SQL
  history, both written with `replaceFileContents()` (`file_util`); tests
  must not use the default path.
- Editing (CSV/TSV and Excel only, `SourceCapabilities::edits`) happens in
  the model: `TableModel::setCells()` copies the cached `RowBlock`s it
  changes (they are shared with readers) and keeps the steps for
  `undo()` / `redo()` and each edited row as read. Sequential sources never
  evict blocks, so the edits stay; the view is not applied again until the
  next `setView()`. Nothing changes outside edit mode
  (`TableBook::setEditing()`), which the UIs turn on only on request.
  `TableBook::saveEdits()` runs `DataSource::saveEdits()` on the worker,
  writing a new file next to the target; the UI then closes the book (a
  file being read cannot be replaced on Windows), moves the file into
  place with `commitSave()` and reopens it. The CSV backend copies the file
  record by record (`rewriteCsv()`), splitting it as csv-parser does after
  the `##` filter, character by character in the file's encoding
  (`textCharacterAt()`), and writes only the edited records anew (in the
  file's encoding with `encodeText()`, compressed by `compress.{hxx,cxx}`,
  BGZF kept); every other byte is kept. Excel edits a fresh `XLDocument`.
  Both check that each edited row still reads as it did.
- CSV/TSV files are converted to UTF-8 while read (`text_encoding.{hxx,cxx}`):
  `detectTextEncoding()` guesses UTF-8, UTF-16 or CP932 from the first
  64 KiB, `OpenOptions::encoding` overrides it, and `TableBook::textEncoding()`
  reports the result. The decoding stream sits between decompression and the
  `##` filter, so the CSV parser only sees UTF-8. The CP932 table
  (`cp932_table.cxx`) is generated by `tools/generate-cp932-table.py`; do
  not edit it by hand.
- Hidden columns (`HiddenColumns` in `cell_format.hxx`) and frozen columns
  (the first N) belong to a view and keep their model column index: the GUI
  gives them a width of 0, the TUI skips them when drawing. Both skip hidden
  columns when moving the cursor and copying, so sorts, filters, search
  hits and statistics are unaffected. The GUI draws frozen cells in the row
  header (right of the row numbers) and their headers in the corner above
  it, which Fl_Table redraws only with the whole table.
- Helpers used by both frontends (cell display text, TSV copy, column
  widths, UTF-8 paths, file stamps, "show in file browser") live in
  `cell_format.{hxx,cxx}` and `file_util.{hxx,cxx}` in `src/common/`.
- The user manual in `docs/` (`en/`, `ja/`, shared `images/`) is built into
  the GUI: `cmake/EmbedFiles.cmake` embeds the Markdown and PNG files at
  build time, `buildHelpSite()` (`help_docs.{hxx,cxx}`, on the Markdown
  subset of `markdown.{hxx,cxx}`) turns them into HTML for Fl_Help_View,
  and `HelpWindow` (`gui_help.cxx`, Help > User Manual) writes the pages
  to a temporary directory and shows them. Headings become `<a name="hN">`
  since Fl_Help_View cuts target names at 31 bytes; links to GitHub
  anchors are resolved to them. `tests/markdown_tests.cxx` converts the
  real manual and checks every link, so keep docs/ within that subset.
  The same pages are also built for web browsers (`buildBrowserManual()`:
  GitHub anchors as heading ids, a style sheet) by the build-time tool
  `tableskim-html-manual` (`src/tools/html_manual.cxx`) into
  `build/<preset>/manual/`, which `cmake/Packaging.cmake` installs
  (Linux: `share/doc/tableskim/html`; macOS and Windows: `Manual/`).
- The GUI layout is defined in `src/gui/gui.fl` (FLUID file, compiled with
  `fltk_wrap_ui`). Edit it with care to keep it loadable by `fluid`;
  generated `gui.h`/`gui.cxx` live in the build directory.
- All `TableWindow`s share one static menu array, and on macOS it is copied
  into the system menu bar, so `TableWindow::sync_menu_items()` (on focus
  and after `open()`) and `requestWindowMenuUpdate()` must run after any
  change to a menu item's value or active state. Find windows with
  `tableWindowOf()` / `frontTableWindow()` (`gui_window_menu.hxx`), never
  by casting `user_data()`: other windows (dialogs) may store other objects
  there. Add a new `TableWindow` to `table_windows` before `show()`, because
  macOS delivers `FL_FOCUS` during `show()`.
- Paths handed over by FLTK are UTF-8; convert with `std::u8string` when
  building `std::filesystem::path` (see `utf8Path` in `file_util.hxx`).
  Non-ASCII file and sheet names must keep working on every platform.

## Coding conventions

Match the existing code:

- C++20, no compiler extensions. Code must build with GCC/Clang on Linux,
  AppleClang on macOS, and MSVC on Windows; avoid POSIX-only APIs in
  `src/common/` (see `common.hxx` for the `ssize_t` shim).
- Files use `.hxx` / `.cxx`, with include guards of the form
  `_FILE_NAME_HXX_`.
- 4-space indentation, Allman braces (opening brace on its own line).
- Types are `PascalCase`; functions and methods are `camelCase`; data
  members are `snake_case_` with a trailing underscore; struct fields and
  locals are `snake_case`.
- File-local helpers go in an anonymous namespace.
- Use `spdlog` for logging and `CLI11` for command-line parsing.
- Comments explain *why*, briefly; keep the comment density of the
  surrounding code. Comments and identifiers are in English.
- No formatter config is committed; do not reformat unrelated code.

## Tests

- Tests use Catch2 v3 (`TEST_CASE`, `SECTION`, `REQUIRE`/`CHECK`).
- Model behaviour is tested against fake `SheetSource` implementations in
  `tests/table_tests.cxx`; backend tests use the files in `demo/data/`.
- Because loading is asynchronous, use the helpers in
  `tests/test_helpers.hxx` (`waitFor`, `waitForCell`, `cellText`) instead of
  sleeping for fixed periods.
- Add or update tests for any change to `src/common/`. Run the full test
  binary before finishing.
- The terminal UI has end-to-end tests in `tests/tui/` (Python `unittest`,
  Linux only): `tui_harness.py` runs `tableskim -t` in a pseudo-terminal and
  reads the screen with pyte; tests send keys and wait for text
  (`wait_text()`, `wait_for()`), never for fixed periods, with `HOME` and
  the XDG directories in a temporary directory. ctest runs them as
  `tableskim-tui-tests` when the configured Python has pyte
  (`TABLESKIM_TUI_TESTS`: `AUTO`; `ON` requires it, as CI does). Install
  `python3-pyte`, or a venv with `tests/tui/requirements.txt` and
  `-DPython3_EXECUTABLE=<venv>/bin/python`. By hand:

  ```sh
  python3 tests/tui/test_tui.py --tableskim build/debug/tableskim \
      --demo-data demo/data -k histogram
  ```

  Add a test there for TUI changes that the Catch2 tests cannot reach.
- The GUI is tested in-process by `tableskim-gui-tests` (`tests/gui/`,
  Linux only), which links `tableskim-gui` and runs under Xvfb: ctest
  starts it with `xvfb-run -a` (`TABLESKIM_GUI_TESTS`: `AUTO` when
  `xvfb-run` is found; `ON` requires it, as CI does). Its `main()` sets
  FLTK up as `runGui()` does and points `XDG_STATE_HOME` at a temporary
  directory. Tests create a `gui_test::Window` (`gui_test_helpers.hxx`),
  call the `TableWindow` methods the menus call, and read the state of
  the widgets and models; they wait with `gui_test::waitFor()`, which
  turns the FLTK loop. Answer a dialog or alert with a `ModalAnswer` set
  up before the call that shows it (`pressButton()`); one left unanswered
  is closed after 10 seconds and the test fails. Views made by a reopen
  may reuse the old ones' addresses: tell a reopen by what the view
  shows. Never run it on the user's display; by hand:

  ```sh
  xvfb-run -a -s "-screen 0 1600x1200x24" ./build/debug/tableskim-gui-tests
  ```

## Git

- Write commit messages in English, imperative mood
  (e.g. "Fix table model edge cases").
- Do not stop at a subject line for anything but a trivial change. Add a
  body, wrapped at about 72 columns and separated from the subject by a
  blank line, that explains *why* the change is needed and what behaviour
  changes (the failure that was fixed, the decision taken). A change with
  several parts gets a bulleted list, one bullet per part. Do not repeat the
  diff.
- Keep the subject short (about 70 characters), without a trailing period.
- If an agent wrote the commit, end the message with the trailers the tool
  provides for attribution (e.g. `Co-Authored-By:`), after a blank line.
- Never commit `build/`, `.cache/`, `CMakeUserPresets.json`, or
  `demo/.venv/`.
