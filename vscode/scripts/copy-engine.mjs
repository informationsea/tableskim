// Copies the engine (tableskim-wasm-node.js and .wasm, built by CMake:
// cmake --build --preset wasm-release --target tableskim-wasm-node) into
// engine/, and the licenses next to package.json, for the package.
// TABLESKIM_WASM_BUILD names another build directory.
import { copyFileSync, existsSync, mkdirSync, readdirSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const extensionDir = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const sourceDir = path.resolve(extensionDir, '..');
const build = path.resolve(extensionDir, process.env.TABLESKIM_WASM_BUILD ?? '../build/wasm-release');

mkdirSync(path.join(extensionDir, 'engine'), { recursive: true });
for (const name of ['tableskim-wasm-node.js', 'tableskim-wasm-node.wasm']) {
    const file = path.join(build, name);
    if (!existsSync(file)) {
        console.error(
            `${file} not found: build it (cmake --build --preset wasm-release --target tableskim-wasm-node) ` +
                'or point TABLESKIM_WASM_BUILD at its build directory',
        );
        process.exit(1);
    }
    copyFileSync(file, path.join(extensionDir, 'engine', name));
}

copyFileSync(path.join(sourceDir, 'LICENSE'), path.join(extensionDir, 'LICENSE'));
// The web app's list (cmake/WebZip.cmake) covers the same code: the core,
// Emscripten's runtime and React.
const zips = path.join(build, 'web-zip');
const thirdParty = existsSync(zips)
    ? readdirSync(zips)
          .map((dir) => path.join(zips, dir, 'THIRD_PARTY_LICENSES.txt'))
          .find((file) => existsSync(file))
    : undefined;
if (thirdParty) copyFileSync(thirdParty, path.join(extensionDir, 'THIRD_PARTY_LICENSES.txt'));
else console.warn('THIRD_PARTY_LICENSES.txt not found: build tableskim-web-zip for the package');
