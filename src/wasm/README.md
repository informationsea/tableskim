# WebAssembly build and web app

A browser-only TableSkim: the core (`tableskim-common`) built with
Emscripten and run in a Web Worker, with a frontend of its own (`web/`),
as the GUI and the TUI share the core. The file never leaves the browser.

- `tableskim-wasm` (`wasm_api.cxx`, `web_session.{hxx,cxx}`,
  `web_json.{hxx,cxx}`) is the module the web app runs in a worker.
- `web/` is the app (React, TypeScript, Tailwind CSS, built with Vite): a
  virtual grid, sheet tabs, sorting, filtering, searching, copying.
- `tableskim-wasm-probe` (`wasm_probe.cxx`, `web/probe/`) was the
  feasibility probe: it opens one file, reads it to the end, sorts it and
  runs an SQL query, timing each step, in Node.js, a browser or natively.

## Building and testing

Install the [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html)
(checked with 6.0.13), then:

```sh
source <emsdk>/emsdk_env.sh
cmake --preset wasm-release            # first time only
cmake --build --preset wasm-release
ctest --preset wasm-release
```

When npm is found (`TABLESKIM_WEB_APP`: `AUTO`, `ON` or `OFF`), the build
also runs `npm ci` (the first time, and after `package-lock.json`
changes) and `npm run build` in `web/`, which type-checks the app and
builds it with the module into `build/wasm-release/web/`. ctest runs
`tableskim-tests` in Node.js, the web app's lint and format checks and, when
Playwright is found at configure time (a package of `web/`: run `npm ci`
there first), its end-to-end tests (`web/tests/app.test.cjs`) in headless
Chromium (`TABLESKIM_WEB_TESTS`: `AUTO`, `ON` or `OFF`). The browser comes
from `npx playwright install chromium` in `web/`. By hand:

```sh
LANG=C.UTF-8 TABLESKIM_WASM_BUILD=build/wasm-release node --test web/tests/app.test.cjs
```

Without a UTF-8 locale, Chromium drops a non-ASCII path given to a file
input. `WebSession` itself is tested natively too
(`tests/web_session_tests.cxx`).

CI does all of this in the `wasm` job of `.github/workflows/ci.yml`
(Emscripten `EMSDK_VERSION` there, ccache, Playwright's Chromium) and
uploads the built app as the `tableskim-web` artifact.

## Trying the web app

```sh
./web.sh              # builds the module and the app, serves them at http://127.0.0.1:8765/
./web.sh --dev        # builds the module, serves web/ with Vite (reloads as web/src changes)
./web.sh --pages      # serves without the isolation headers, as GitHub Pages does
./web.sh --help       # --debug (wasm-debug), --port, --open, --no-build
```

`web.sh` sources `emsdk_env.sh` from `$EMSDK` or `~/emsdk` when `emcc` is
not on the PATH, configures the preset once (with `TABLESKIM_WEB_APP=ON`)
and builds only what it serves, not the tests. The page has to be opened
as `127.0.0.1` or `localhost` (or over HTTPS): elsewhere a browser does not
make it cross-origin isolated, and the engine cannot start.

## Working on the web app

```sh
cd web
npm ci                  # once
npm run dev             # http://localhost:5173/, reloads as the sources change
npm run typecheck
npm run lint            # ESLint
npm run format          # Prettier (format:check only checks)
npm run build           # into web/dist/, with the module
npm run preview         # serves web/dist/
```

The web app's code follows Prettier (`.prettierrc.json`: 4 spaces, 120
columns, Tailwind classes sorted by its plugin), not the C++ code's
Allman braces; ESLint (`eslint.config.js`) checks types, the React hooks
rules and promises left unhandled. ctest runs both checks
(`tableskim-web-lint`, `tableskim-web-format`). TypeScript stays on 6.0,
the last version with the API typescript-eslint uses (7 is native).

The module comes from `../build/wasm-release` (`TABLESKIM_WASM_BUILD`
names another build directory): `vite.config.ts` serves `tableskim-wasm.js`
and `.wasm` as they are in development and copies them next to the app on
build. They are not bundled: the module is the script of the worker that
runs it, and Emscripten starts its threads from that same URL.

The page must be cross-origin isolated, which threads need
(`SharedArrayBuffer`): served with `Cross-Origin-Opener-Policy:
same-origin` and `Cross-Origin-Embedder-Policy: require-corp`, as
`npm run dev`, `npm run preview` and `web/serve.mjs` do. Any static server
that sends them will do. The app uses relative URLs, so it works from any
directory of a host.

Hosts that cannot send the headers, such as GitHub Pages, are covered by a
service worker the app carries (`web/public/coi-serviceworker.js`, next to
`index.html` in the built app): when the page is not isolated,
`src/isolation.ts` registers it and loads the page once more, now with the
headers the worker adds to every response of the app's directory (the
page, its scripts, the module and the threads' workers). Where the server
sends the headers, it is not registered. It needs a secure context (HTTPS,
or `localhost` / `127.0.0.1`); without one, or if a reload did not help,
the page explains what is missing. To try it, `./web.sh --pages` (or
`node web/serve.mjs --no-isolation-headers ...`) serves without the
headers, and an end-to-end test does the same. To publish the app, copy
`build/wasm-release/web/` (or `web/dist/`) to the host as it is.

`node web/serve.mjs build/wasm-release` serves the app CMake built (or
`web/dist/`) at http://127.0.0.1:8765/ and the probe page at `/probe/`.

Sources (`web/src/`): `protocol.ts` types the messages, `client.ts` sends
them, `grid/controller.ts` is the virtual grid's state and behaviour
(viewport, row cache, selection, keyboard, mouse) and `grid/Grid.tsx`
draws it; `App.tsx` holds the app's state and `components/` the rest of
the page. Colours are tokens in `index.css` (light and dark), used through
Tailwind's utilities. The end-to-end tests find elements by the ids and
classes the app gives them (`#file-input`, `.cell[data-row]`, `.header`,
`#status-rows`, ...): keep them when changing the markup.

The probe in Node.js sees the host file system under `/host`; in Chromium,
`web/probe/run-browser.cjs` runs the probe page through Playwright:

```sh
node build/wasm-release/tableskim-wasm-probe.js /host$PWD/demo/data/normal.parquet \
    'SELECT City, count(*) FROM normal GROUP BY City'
LANG=C.UTF-8 node web/probe/run-browser.cjs build/wasm-release demo/data/normal.xlsx
```

## How it runs

- Every object is built with `-pthread -fwasm-exceptions` (set before the
  dependencies are added). `TableBook`'s worker is a pthread, so the core
  is unchanged.
- `-sPROXY_TO_PTHREAD`: `main()` runs on a thread of its own. The thread
  running the runtime (the worker's own) is the only one that may post to
  the page and touch the file systems, and the core's threads read files
  through it; it must never wait for them. So `main()` is the session
  thread: it takes the requests the worker received from a queue, runs
  them on a `WebSession` and hands the replies back to the runtime thread
  (`emscripten_proxy_async`), which posts them to the page.
- The module is the worker's own script: Emscripten starts its threads from
  the script of the worker that starts them. `wasm_worker_pre.js` receives
  the page's messages; an "open" request carries the `File`, which it
  mounts with WORKERFS (read in place, in slices, not copied into memory)
  and replaces with its path. The mounts of earlier files go once the next
  "open" is answered, as the session closes the book first.
- `TableModel` never blocks: a "rows" reply marks cells that are still
  loading, and the model's events reach the page as `{sheet: bits}`
  (`WebSession::Event`), coalesced on the runtime thread; the page then
  asks again for what it shows.
- DuckDB cannot run the program it builds to name its platform, so
  `cmake/Dependencies.cmake` names it (`wasm_threads`, as duckdb-wasm does).
- The TUI, the GUI and the HTML manual tool are not built.

## Protocol

The page posts `{id, method, params}`; the reply is `{id, result}` or
`{id, error}` (`web/src/client.ts` wraps this in typed promises;
`web/src/protocol.ts` has the types). Rows and columns are 0-based;
`sheet` is the id a "sheet" request returned.

| Method | Params | Result |
| --- | --- | --- |
| `open` | `file` (a `File`), `header` (`auto`, `yes`, `no`), `encoding` | `sheets`, `usesHeaderOption`, `isTextFormat`, `supportsSql`, `textEncoding` |
| `close` | | |
| `sheet` | `name` | the sheet's info (below), with its `sheet` id |
| `info` | `sheet` | `state` (`opening`, `ready`, `error`), `error`, `rows`, `rowsExact`, `displayRows`, `rowsText`, `hasHeader`, `columns` (`name`, `mark`), `capabilities`, `view`, `pending`, `filterText`, `viewError` (once), `checkpoint`, `readingStopped` |
| `rows` | `sheet`, `first`, `last`, `firstColumn`, `lastColumn` | `first`, `firstColumn`, `rows` (display texts), `kinds` (a character per cell: `n` number, `s` other, `e` empty, `l` loading, `o` beyond the row, `x` error), `loading`, `end` |
| `setView` | `sheet`, `sort` (`column`, `ascending`), `filter` (`combine`, `conditions`: `column`, `op`, `value`) or null | `view` |
| `sortCycle` | `sheet`, `column` | `view` (by the column ascending, descending, then none) |
| `search` / `cancelSearch` / `searchState` | `sheet`, `text`, `caseSensitive`, `regex`, `column` | `active`, `done`, `truncated`, `error`, `scannedRows`, `hits`, `generation` |
| `findHit` | `sheet`, `row`, `column`, `forward`, `includeFrom` | `index` (null if none), `row`, `column`, `hits`, `wrapped`, `pending` |
| `continueReading` / `stopReading` | `sheet` | |
| `copy` | `sheet`, `top`, `left`, `bottom`, `right` | `text` (TSV), `copiedRows`, `skippedRows` |
| `filterOps` | | `op`, `label`, `takesValue` for each filter operator |

## Probe results

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

## Findings from the probe

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
  runs the module; the probe's `main()` and `wasm_probe_pre.js` showed the
  pieces (posting output, mounting the `File`), which `tableskim-wasm`
  now uses.

## Not done yet

SQL, statistics, histograms, scatter plots, Group By, exports and editing
(the core has them; the session and the app do not ask for them yet);
publishing on the project's GitHub Pages site (the app is ready for it),
offline use and file handlers (PWA); loading DuckDB only
when needed. Follow cannot work on a `File`, which is a snapshot.
