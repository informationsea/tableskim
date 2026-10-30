// End-to-end tests of the VS Code extension's webview (vscode.html, built
// into ../vscode/media by `npm run build:vscode`) in headless Chromium, with
// the engine in Node.js as the extension runs it (tableskim-wasm-node.js in
// a worker_threads Worker). This test stands in for the extension: it
// relays the page's messages as vscode/src/editor.ts does.
//   TABLESKIM_WASM_BUILD=build/wasm-release node --test web/tests/vscode.test.cjs
const { after, before, test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { Worker } = require('node:worker_threads');
const { chromium } = require('playwright');

const root = path.join(__dirname, '..', '..');
const buildDir = path.resolve(root, process.env.TABLESKIM_WASM_BUILD ?? 'build/wasm-release');
const media = path.resolve(root, process.env.TABLESKIM_VSCODE_MEDIA ?? 'vscode/media');
const demo = (name) => path.join(root, 'demo', 'data', name);
const ORIGIN = 'https://webview.test';

let browser;

before(async () => {
    browser = await chromium.launch({ executablePath: process.env.TABLESKIM_CHROMIUM });
});

after(async () => {
    await browser?.close();
});

// The page as the extension serves it: a CSP, a <base> and the file's name.
function webviewHtml(fileName) {
    const page = fs.readFileSync(path.join(media, 'vscode.html'), 'utf8');
    const csp = `default-src 'none'; script-src ${ORIGIN}; style-src ${ORIGIN} 'unsafe-inline'; img-src ${ORIGIN} blob: data:; font-src ${ORIGIN}`;
    const head =
        `<meta http-equiv="Content-Security-Policy" content="${csp}">` +
        `<base href="${ORIGIN}/media/">` +
        `<meta name="tableskim-file" content="${fileName}">`;
    return page.replace('<head>', `<head>${head}`).replace(/ crossorigin/g, '');
}

// Opens the webview on a file; returns the page and what it sent the extension.
async function openWebview(file) {
    const context = await browser.newContext();
    const page = await context.newPage();
    const errors = [];
    page.on('pageerror', (error) => errors.push(error.message));
    const sent = [];
    const engine = new Worker(path.join(buildDir, 'tableskim-wasm-node.js'));
    // Bytes cross page.evaluate() as arrays.
    const toPage = (message) =>
        page
            .evaluate((data) => {
                if (data.message.type === 'file') data.message.data = new Uint8Array(data.message.data).buffer;
                window.postMessage(data, '*');
            }, message)
            .catch(() => {});
    engine.on('message', (message) => {
        if (message.type === 'file') message = { ...message, data: [...new Uint8Array(message.data)] };
        void toPage({ type: 'engine', message });
    });
    await page.exposeFunction('toExtension', (message) => {
        sent.push(message);
        if (message.type !== 'engine') return;
        const request = message.message;
        if (request.method === 'open') request.params = { ...request.params, path: file };
        engine.postMessage(request);
    });
    await page.addInitScript(() => {
        window.acquireVsCodeApi = () => ({
            postMessage: (message) =>
                void window.toExtension(
                    message.data instanceof Uint8Array ? { ...message, data: [...message.data] } : message,
                ),
        });
    });
    await page.route(`${ORIGIN}/**`, (route) => {
        const url = new URL(route.request().url());
        if (url.pathname === '/')
            return route.fulfill({ contentType: 'text/html', body: webviewHtml(path.basename(file)) });
        const name = path.join(media, decodeURIComponent(url.pathname.replace(/^\/media\//, '')));
        const type = name.endsWith('.js') ? 'text/javascript' : name.endsWith('.css') ? 'text/css' : 'text/plain';
        return route.fulfill({ contentType: type, body: fs.readFileSync(name) });
    });
    await page.goto(`${ORIGIN}/`);
    const close = async () => {
        await context.close();
        await engine.terminate();
        assert.deepEqual(errors, []);
    };
    return { page, sent, close };
}

const cell = (page, row, column) => page.locator(`.cell[data-row="${row}"][data-column="${column}"]`);

test('the webview opens its document through the extension', async () => {
    const { page, close } = await openWebview(demo('normal.csv'));
    try {
        await page.waitForSelector('body[data-ready=true]');
        await page.waitForSelector('.cell:not(.k-l)');
        await page.locator('#status-rows').filter({ hasText: '54 rows' }).waitFor();
        assert.notEqual(await cell(page, 0, 0).innerText(), '');
    } finally {
        await close();
    }
});

test('the webview opens a file whose name is not ASCII', async () => {
    const { page, close } = await openWebview(demo('多言語サンプル.parquet'));
    try {
        await page.waitForSelector('.cell:not(.k-l)');
        assert.equal(await page.locator('#sql-toggle').isEnabled(), true);
    } finally {
        await close();
    }
});

test('the webview hands exports and Open… to the extension', async () => {
    const { page, sent, close } = await openWebview(demo('normal.csv'));
    try {
        await page.waitForSelector('.cell:not(.k-l)');
        await page.locator('#export-button').click();
        await page.locator('#export-start').click();
        await page.locator('#export-status').filter({ hasText: 'Saved normal-export.csv' }).waitFor();
        const save = sent.find((message) => message.type === 'save');
        assert.equal(save.name, 'normal-export.csv');
        const text = Buffer.from(save.data).toString('utf8');
        const source = fs.readFileSync(demo('normal.csv'), 'utf8');
        assert.equal(text.split(/\r?\n/)[0], source.split(/\r?\n/)[0]);
        assert.equal(text.trim().split(/\r?\n/).length, source.trim().split(/\r?\n/).length);

        await page.keyboard.press('Escape');
        await page.locator('#export-dialog').waitFor({ state: 'hidden' });
        await page.locator('#open-button').click();
        while (!sent.some((message) => message.type === 'pick'))
            await new Promise((resolve) => setTimeout(resolve, 20));
    } finally {
        await close();
    }
});
