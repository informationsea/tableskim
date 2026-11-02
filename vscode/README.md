# TableSkim for Visual Studio Code (prototype)

TableSkim as a custom editor in VS Code: CSV/TSV/VCF (optionally
compressed), Excel, Parquet, DuckDB, SQLite and JSON / NDJSON files,
with sorting, filters, search, statistics, histograms, scatter plots,
Group By, SQL and exports, as in the [web app](../src/wasm/README.md).

- Parquet, Excel, DuckDB and SQLite files open in TableSkim by default.
- CSV, TSV, VCF, JSON and compressed files open as text. To open them in
  TableSkim, choose **Open With… > TableSkim** or **Open in TableSkim** from
  the Explorer's context menu.
- Exports and chart images are saved where you choose in a save dialog.
- When the file changes on disk, TableSkim asks whether to read it again.
  **TableSkim: Reload File** (the refresh button in the editor's title bar)
  reads it again at any time, and so does **Reload** in the message shown
  if the engine stops.

It works with local files and, with Remote Development (SSH, WSL, Dev
Containers), with files on the remote machine, where the extension runs.
VS Code for the Web (vscode.dev) is not supported.

## How it works

```
[webview: the web app's frontend]  ⇄ postMessage ⇄  [extension host (Node.js)]
  web/src/vscode/                                     src/editor.ts
                                                       └ worker_threads Worker:
                                                         tableskim-wasm-node.js
                                                         (the core, WebAssembly;
                                                          files read in place by NODEFS)
```

- The frontend is the web app's (`web/src/`), built for the webview from
  `web/vscode.html` and `web/src/vscode/` (`npm run build:vscode` in `web/`)
  into `media/`. `web/src/vscode/port.ts` relays the engine's protocol
  (`web/src/protocol.ts`) through the extension; `web/src/platform.ts`
  hands exports to it and hides Open… (the editor shows its document
  only).
- The engine is the web app's module built for Node.js (the CMake target
  `tableskim-wasm-node`; `src/wasm/wasm_worker_pre.js`). Its threads need
  `SharedArrayBuffer`, which a webview, not being cross-origin isolated,
  does not have, but Node.js has. It runs in a worker of its own, one per
  editor, so that the extension host is not kept busy.
- An "open" names the document's path: the extension sets it, whatever the
  page asks for, and the engine mounts its directory with NODEFS.
- Each load of the page gets an engine of its own: the page says
  `{type: 'hello'}` once it listens, and the extension then starts a new
  engine, stopping the last one and dropping what it still posts. So a page
  that VS Code loads again (Developer: Reload Webviews, a crashed webview)
  starts over, and reloading a file is loading the page again.
- The extension watches the document's directory (not a glob of the file's
  name, which may hold glob characters) and, after a burst of changes to the
  file, asks once whether to reload it, as the engine reads it in place.
- Not there yet, as in the web app: editing and Follow.

## Building

Build the engine with Emscripten first (see `src/wasm/README.md`), then:

```sh
source <emsdk>/emsdk_env.sh
cmake --preset wasm-release
cmake --build --preset wasm-release --target tableskim-wasm-node tableskim-web-zip
(cd web && npm ci)
cd vscode
npm ci
npm run build          # engine/, media/ and out/
npm test               # the engine and the editor provider in Node.js
npm run package        # tableskim-<version>-vscode.vsix
```

`tableskim-web-zip` is only needed for `THIRD_PARTY_LICENSES.txt` in the
package. `TABLESKIM_WASM_BUILD` names another build directory.

The webview's end-to-end test (`web/tests/vscode.test.cjs`) runs the built
page in headless Chromium with the engine in Node.js, relaying messages as
the extension does:

```sh
LANG=C.UTF-8 node --test web/tests/vscode.test.cjs
```

`npm run test:vscode` runs the extension in VS Code itself (downloaded by
`@vscode/test-electron` into `.vscode-test/`, which needs
`update.code.visualstudio.com` and `vscode.download.prss.microsoft.com`):
`tests/integration/suite.cjs` opens demo files in the custom editor and
waits until the webview has asked the engine for rows and got them,
watching the messages the extension relays (`activate()` returns
`onDidRelay` for this). It also reloads pages, saves an export and changes a
file on disk: in VS Code's extension test mode only, `activate()` returns
`testing` too, which answers the save dialog and the reload question, and
the page gets `<meta name="tableskim-test">`, on which it clicks elements
and reads their text when the extension asks (`web/src/vscode/testHook.ts`). On Linux without a display:

```sh
xvfb-run -a npm run test:vscode
```

The package takes the project's version (`CMakeLists.txt`), not the one
in `package.json`. CI (the `wasm` job of `.github/workflows/ci.yml`)
builds the extension, runs all three tests, the one in VS Code under
`xvfb-run` (VS Code cached by its version, which
`TABLESKIM_VSCODE_VERSION` gives the test), and uploads the package as the `tableskim-vscode` artifact;
the release attaches `tableskim-<version>-vscode.vsix`.

To try the extension, open this directory in VS Code and run **Run >
Start Debugging** (Extension Development Host), or install the VSIX with
**Extensions: Install from VSIX…**.
