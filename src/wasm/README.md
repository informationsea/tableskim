# WebAssembly build (feasibility probe)

The plan is a browser-only TableSkim: the core (`tableskim-common`) built
with Emscripten and run in a Web Worker, with a frontend of its own, as the
GUI and the TUI share the core today. This directory holds the first step,
which checks that the core builds and runs as WebAssembly:

- the `wasm-debug` / `wasm-release` presets build `tableskim-common` and
  `tableskim-tests` with Emscripten (threads, wasm exceptions), and ctest
  runs the tests in Node.js;
- `tableskim-wasm-probe` (`wasm_probe.cxx`) opens one file through the
  normal backends, reads it to the end, sorts it and runs an SQL query,
  timing each step. It runs in Node.js, in a browser (`web/probe/`) and,
  for comparison, natively.

## Building and testing

Install the [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html)
(checked with 6.0.13), then:

```sh
source <emsdk>/emsdk_env.sh
cmake --preset wasm-release            # first time only
cmake --build --preset wasm-release
ctest --preset wasm-release            # tableskim-tests in Node.js
```

The probe in Node.js sees the host file system under `/host`:

```sh
node build/wasm-release/tableskim-wasm-probe.js /host$PWD/demo/data/normal.parquet \
    'SELECT City, count(*) FROM normal GROUP BY City'
```

In a browser, `web/probe/serve.mjs` serves the page with the headers that
make it cross-origin isolated (needed for threads); open it and pick a file.
`web/probe/run-browser.cjs` does the same in headless Chromium through
Playwright:

```sh
node web/probe/serve.mjs build/wasm-release            # http://127.0.0.1:8765/
LANG=C.UTF-8 node web/probe/run-browser.cjs build/wasm-release demo/data/normal.xlsx
```

Without a UTF-8 locale, Chromium drops a non-ASCII path given to the file
input; the harness then stops with a message.

## How it runs

- Every object is built with `-pthread -fwasm-exceptions` (set before the
  dependencies are added). `TableBook`'s worker is a pthread, so the core
  is unchanged; threads need `SharedArrayBuffer`, i.e. a page served with
  `Cross-Origin-Opener-Policy: same-origin` and
  `Cross-Origin-Embedder-Policy: require-corp`.
- `-sPROXY_TO_PTHREAD`: `main()` runs on a thread of its own, so that the
  thread running the runtime stays free to start more threads and to serve
  the file systems, which only it may touch.
- In a browser the module is the worker's own script (Emscripten starts
  its threads from the script of the worker that starts them). The page
  posts the `File`; `wasm_probe_pre.js` mounts it under `/input` with
  WORKERFS, which reads slices of it as needed instead of copying it into
  memory.
- DuckDB cannot run the program it builds to name its platform, so
  `cmake/Dependencies.cmake` names it (`wasm_threads`, as duckdb-wasm does).
- The TUI, the GUI and the HTML manual tool are not built.

## Results

4-core cloud container, Release build, Chromium 153 (headless, Playwright
1.63), Node.js 22. Times are seconds as the probe reports them.

All of `tableskim-tests` passes in Node.js (303 test cases, the same as
natively). In Chromium, every demo format opens, reads, sorts and (where
supported) runs SQL: CSV/TSV (UTF-8, UTF-16, CP932, gzip/bzip2/xz/zstd),
Excel (merged cells, several sheets, dates), Parquet, JSON/NDJSON (gzip,
nested), SQLite and DuckDB files, and non-ASCII file names.

A generated CSV of 1,000,000 rows x 8 columns (68 MiB; also gzip-compressed
and as Parquet, 22 MiB), reading every row (`read all`, continuing at each
memory checkpoint) and sorting by the first column:

| File        | Step       | Native | Node.js (wasm) | Chromium (wasm) |
| ----------- | ---------- | -----: | -------------: | --------------: |
| big.csv     | first rows |   0.17 |           0.17 |            0.35 |
|             | read all   |   1.55 |           1.82 |            3.22 |
|             | sort       |   1.01 |           1.10 |            1.28 |
| big.csv.gz  | read all   |   1.79 |           2.05 |     2.79 [^gz]  |
| big.parquet | open       |   0.02 |           0.27 |            0.25 |
|             | read all   |   0.90 |           0.84 |            1.17 |
|             | sort       |   0.10 |           0.22 |            0.75 |
|             | SQL        |   0.84 |           1.26 |            1.44 |

[^gz]: 21.3 s before `DecompressingBuffer` gave its `std::ifstream` a
    256 KiB buffer. With libc++'s default 4 KiB one, each 256 KiB refill
    became 64 reads, and in a browser each read of a `File` is a
    `File.slice()` plus a hop from the reading thread to the one serving the
    file system.

- **Speed**: reading and sorting in memory takes 1.1-2x as long as
  natively, Chromium being the slower; DuckDB takes about 0.25 s more to
  open a file, and its sorts and queries take 1.5-7x as long. Loading the
  module from a local server and running the probe on a small file takes
  about 0.4 s.
- **Memory**: WebAssembly memory only grows. The CSV above takes 287 MiB of
  it (cells are smaller than natively: 32-bit pointers make 100 MiB of
  cells 483,328 rows here, 295,424 natively), the Parquet file 155 MiB
  after the query. The 4 GiB limit of wasm32 leaves room for roughly
  10-14 million such rows.
- **Size**: `tableskim-wasm-probe.wasm` is 29.2 MiB (7.4 MiB gzip,
  4.9 MiB brotli); DuckDB with its Parquet and JSON extensions is about
  88% of the code linked in (by the size of the static libraries).

## Findings for the next phases

- No change to the core's threading was needed; keep pthreads and serve
  the app cross-origin isolated (`coi-serviceworker` where headers cannot
  be set, e.g. GitHub Pages).
- Read files in large pieces: small reads are cheap natively but not
  through WORKERFS. The other `std::ifstream` reads in `src/common/` are
  one-off heads or only used by Follow, which the browser cannot offer.
- `size_t` is 32 bits: brace-initialising a `std::string_view` from
  DuckDB's 64-bit `idx_t` failed to compile (`table_duckdb.cxx`).
- DuckDB dominates the download. Loading it only for Parquet, JSON, DuckDB
  files and SQL means a second module (or dynamic linking) and moving
  `table_registry.cxx`'s registration behind it.
- The bridge to a frontend should be a message-based API in the worker that
  runs the module; the probe's `main()` and `wasm_probe_pre.js` show the
  pieces (posting output, mounting the `File`).
