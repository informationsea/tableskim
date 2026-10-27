import { createReadStream, existsSync, readFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import tailwindcss from '@tailwindcss/vite';
import react from '@vitejs/plugin-react';
import { defineConfig, type Plugin } from 'vite';

const webDir = path.dirname(fileURLToPath(import.meta.url));
// The CMake build directory holding tableskim-wasm.js and .wasm.
const wasmBuild = path.resolve(webDir, process.env.TABLESKIM_WASM_BUILD ?? '../build/wasm-release');
const MODULE_FILES = ['tableskim-wasm.js', 'tableskim-wasm.wasm'];

// SharedArrayBuffer, which the core's threads need, is only there in a
// cross-origin isolated page.
const ISOLATION_HEADERS = {
    'Cross-Origin-Opener-Policy': 'same-origin',
    'Cross-Origin-Embedder-Policy': 'require-corp',
};

function moduleFile(name: string): string
{
    const file = path.join(wasmBuild, name);
    if (!existsSync(file))
        throw new Error(`${file} not found: build tableskim-wasm (cmake --build --preset wasm-release) ` +
                        'or point TABLESKIM_WASM_BUILD at its build directory');
    return file;
}

// The WebAssembly module is not bundled: it is the script of the worker
// that runs it, and Emscripten starts its threads from that same URL. It
// is served as it is in development and copied next to the app on build.
function tableskimWasm(): Plugin
{
    return {
        name: 'tableskim-wasm',
        configureServer(server)
        {
            server.middlewares.use((request, response, next) =>
            {
                const name = path.posix.basename(new URL(request.url ?? '/', 'http://localhost').pathname);
                if (!MODULE_FILES.includes(name))
                    return next();
                // The worker needs the headers too, or it is not isolated:
                // this answers before Vite's own middleware adds them.
                for (const [header, value] of Object.entries(ISOLATION_HEADERS))
                    response.setHeader(header, value);
                response.setHeader('Content-Type', name.endsWith('.wasm') ? 'application/wasm' : 'text/javascript');
                createReadStream(moduleFile(name)).pipe(response);
            });
        },
        generateBundle()
        {
            for (const name of MODULE_FILES)
                this.emitFile({ type: 'asset', fileName: name, source: readFileSync(moduleFile(name)) });
        },
    };
}

export default defineConfig({
    // Relative URLs, so that the app works from any directory of a host.
    base: './',
    plugins: [react(), tailwindcss(), tableskimWasm()],
    server: { headers: ISOLATION_HEADERS },
    preview: { headers: ISOLATION_HEADERS },
    build: { chunkSizeWarningLimit: 1000 },
});
