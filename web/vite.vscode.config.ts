import path from 'node:path';
import { fileURLToPath } from 'node:url';
import tailwindcss from '@tailwindcss/vite';
import react from '@vitejs/plugin-react';
import { defineConfig } from 'vite';

const webDir = path.dirname(fileURLToPath(import.meta.url));

// The VS Code extension's webview (vscode.html, src/vscode/), built into
// the extension (vscode/media/, or TABLESKIM_VSCODE_MEDIA). The engine runs
// in the extension host, so the module is not part of it.
export default defineConfig({
    // The extension loads the page with a <base> of the directory's webview URI.
    base: './',
    plugins: [react(), tailwindcss()],
    // public/ holds the browser's service worker, of no use in a webview.
    publicDir: false,
    build: {
        outDir: path.resolve(webDir, process.env.TABLESKIM_VSCODE_MEDIA ?? '../vscode/media'),
        emptyOutDir: true,
        rollupOptions: { input: path.resolve(webDir, 'vscode.html') },
        chunkSizeWarningLimit: 1000,
    },
});
