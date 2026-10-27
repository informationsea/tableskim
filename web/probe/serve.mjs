// Serves the probe page with the headers that make it cross-origin
// isolated, which SharedArrayBuffer (and so threads) needs:
//   node web/probe/serve.mjs <build dir> [port]
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { dirname, extname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const pageDir = dirname(fileURLToPath(import.meta.url));
const buildDir = process.argv[2];
const port = Number(process.argv[3] ?? 8765);
if (!buildDir)
{
    console.error('usage: node serve.mjs <build dir> [port]');
    process.exit(2);
}

const files = {
    '/': [pageDir, 'index.html'],
    '/index.html': [pageDir, 'index.html'],
    '/tableskim-wasm-probe.js': [buildDir, 'tableskim-wasm-probe.js'],
    '/tableskim-wasm-probe.wasm': [buildDir, 'tableskim-wasm-probe.wasm'],
};
const types = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript', '.wasm': 'application/wasm' };

createServer(async (request, response) =>
{
    const entry = files[new URL(request.url, 'http://localhost').pathname];
    if (!entry)
    {
        response.writeHead(404).end();
        return;
    }
    try
    {
        const body = await readFile(join(...entry));
        response.writeHead(200, {
            'Content-Type': types[extname(entry[1])],
            'Cross-Origin-Opener-Policy': 'same-origin',
            'Cross-Origin-Embedder-Policy': 'require-corp',
        });
        response.end(body);
    }
    catch (error)
    {
        response.writeHead(500).end(String(error));
    }
}).listen(port, '127.0.0.1', () => console.log(`http://127.0.0.1:${port}/`));
