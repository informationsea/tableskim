// Serves the web app (web/app/ at /) and the probe page (web/probe/ at
// /probe/) with the module files of a WebAssembly build, adding the
// headers that make the pages cross-origin isolated, which
// SharedArrayBuffer (and so threads) needs:
//   node web/serve.mjs <build dir> [port]
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { basename, dirname, extname, join, normalize, sep } from 'node:path';
import { fileURLToPath } from 'node:url';

const webDir = dirname(fileURLToPath(import.meta.url));
const buildDir = process.argv[2];
const port = Number(process.argv[3] ?? 8765);
if (!buildDir)
{
    console.error('usage: node web/serve.mjs <build dir> [port]');
    process.exit(2);
}

// Built by CMake; served next to whichever page loads them.
const BUILD_FILES = new Set([
    'tableskim-wasm.js',
    'tableskim-wasm.wasm',
    'tableskim-wasm-probe.js',
    'tableskim-wasm-probe.wasm',
]);
const TYPES = {
    '.html': 'text/html; charset=utf-8',
    '.js': 'text/javascript; charset=utf-8',
    '.css': 'text/css; charset=utf-8',
    '.wasm': 'application/wasm',
};

// The file for a URL path, or null.
function resolve(pathname)
{
    if (BUILD_FILES.has(basename(pathname)))
        return join(buildDir, basename(pathname));
    let root = join(webDir, 'app');
    let rest = pathname;
    if (pathname === '/probe' || pathname.startsWith('/probe/'))
    {
        root = join(webDir, 'probe');
        rest = pathname.slice('/probe'.length);
    }
    if (rest === '' || rest.endsWith('/'))
        rest += 'index.html';
    const file = normalize(join(root, decodeURIComponent(rest)));
    return file.startsWith(root + sep) ? file : null;
}

createServer(async (request, response) =>
{
    const file = resolve(new URL(request.url, 'http://localhost').pathname);
    try
    {
        if (!file)
            throw Object.assign(new Error('outside the served directories'), { code: 'ENOENT' });
        const body = await readFile(file);
        response.writeHead(200, {
            'Content-Type': TYPES[extname(file)] ?? 'application/octet-stream',
            'Cache-Control': 'no-cache',
            'Cross-Origin-Opener-Policy': 'same-origin',
            'Cross-Origin-Embedder-Policy': 'require-corp',
        });
        response.end(body);
    }
    catch (error)
    {
        response.writeHead(error.code === 'ENOENT' || error.code === 'EISDIR' ? 404 : 500).end();
    }
}).listen(port, '127.0.0.1', () => console.log(`http://127.0.0.1:${port}/`));
