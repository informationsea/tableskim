# TableSkim for VS Code: development

How the extension (`vscode/`) works, and how to build, test and package
it. What users read is [README.md](README.md), shown on the Marketplace
and in VS Code's Extensions view; [CHANGELOG.md](CHANGELOG.md) is shown
there too.

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
the `wasm` job runs the test in VS Code again in the oldest version the
extension claims to support (`engines.vscode` in `package.json`), and its
`vscode` job runs `npm test` and `npm run test:vscode` on Windows and
macOS with what the `wasm` job built; the release attaches
`tableskim-<version>-vscode.vsix`. Raise `engines.vscode` (and
`@types/vscode` with it) rather than drop that test.

To try the extension, open this directory in VS Code and run **Run >
Start Debugging** (Extension Development Host), or install the VSIX with
**Extensions: Install from VSIX…**.

## Package contents

The package (`.vscodeignore`) holds `out/`, `engine/`, `media/`,
`README.md`, `CHANGELOG.md`, `icon.png` and the licenses. `icon.png` is
the GUI's icon (`src/images/icon.png`) and `LICENSE` the project's, both
copied by `npm run build:engine` like `THIRD_PARTY_LICENSES.txt`.
`images/` stays out: the README's images are taken from GitHub
(`npm run package` passes `--baseImagesUrl` and `--baseContentUrl` for the
README's relative links), so a screenshot changed after a release shows
in that release's README too.

Add what a release changes for users to `CHANGELOG.md` under
`## Unreleased`, and rename that heading to the version before tagging:
`npm run package` warns when the version it packs has no heading.

## Publishing

The release workflow (`.github/workflows/release.yml`, job
`publish-vscode`) publishes the package it attaches, after the GitHub
release is made:

- to the Visual Studio Marketplace when the repository has the secret
  `VSCE_PAT`: an Azure DevOps personal access token with the Marketplace
  scope "Manage", of an account that is a member of the publisher
  `informationsea` (created at
  <https://marketplace.visualstudio.com/manage>);
- to [Open VSX](https://open-vsx.org/) (VSCodium and other editors) when it
  has `OVSX_PAT`, a token of an Open VSX account that owns the namespace
  `informationsea`.

Without either secret it publishes nothing. It first checks that
`CHANGELOG.md` has a heading of the tag's version, and fails otherwise
(the GitHub release is already made then; publish by hand after fixing
the changelog with `npx vsce publish --packagePath <vsix>`). The
Marketplace takes each version once, and the extension's version is the
project's: an extension-only fix needs a new release of the project.
The package is marked `"preview": true`, which the Marketplace shows.

## Screenshots

`scripts/screenshots.cjs` takes the README's screenshots
(`images/vscode-*.png`) in VS Code itself, the version the integration
test uses, driven by Playwright (a package of `web/`, so `npm ci` there
first). Build first; on Linux without a display:

```sh
xvfb-run -a -s "-screen 0 1280x800x24" node scripts/screenshots.cjs
```

`TABLESKIM_SCREENSHOTS=vscode-main.png,...` takes only those. Look at
each image before committing it.
