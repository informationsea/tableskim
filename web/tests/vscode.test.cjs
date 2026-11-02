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
    const saves = [];
    const saveWaiters = [];
    const takeSave = () =>
        saves.length ? Promise.resolve(saves.shift()) : new Promise((resolve) => saveWaiters.push(resolve));
    let engine = null;
    let engines = 0;
    // Bytes cross page.evaluate() as arrays.
    const toPage = (message) =>
        page
            .evaluate((data) => {
                if (data.type === 'engine' && data.message.type === 'file')
                    data.message.data = new Uint8Array(data.message.data).buffer;
                window.postMessage(data, '*');
            }, message)
            .catch(() => {});
    // As the extension: an engine for each load of the page, the old one's messages dropped.
    const startEngine = () => {
        void engine?.terminate();
        const current = new Worker(path.join(buildDir, 'tableskim-wasm-node.js'));
        engine = current;
        engines++;
        current.on('message', (message) => {
            if (engine !== current) return;
            if (message.type === 'file') message = { ...message, data: [...new Uint8Array(message.data)] };
            void toPage({ type: 'engine', message });
        });
    };
    await page.exposeFunction('toExtension', (message) => {
        sent.push(message);
        if (message.type === 'save') {
            const waiter = saveWaiters.shift();
            if (waiter) waiter(message);
            else saves.push(message);
        }
        if (message.type === 'hello') startEngine();
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
        await engine?.terminate();
        assert.deepEqual(errors, []);
    };
    const completeSave = (id, result) => toPage({ type: 'saveResult', id, ...result });
    return { page, sent, takeSave, completeSave, close, engines: () => engines };
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

test('a reloaded webview gets a new engine and shows its document again', async () => {
    const { page, close, engines } = await openWebview(demo('normal.csv'));
    try {
        await page.waitForSelector('.cell:not(.k-l)');
        assert.equal(engines(), 1);
        // As Developer: Reload Webviews: the page starts over, the extension does not.
        await page.reload();
        await page.waitForSelector('.cell:not(.k-l)');
        await page.locator('#status-rows').filter({ hasText: '54 rows' }).waitFor();
        assert.equal(engines(), 2);
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

test('the webview hands exports to the extension and has no Open…', async () => {
    const { page, takeSave, completeSave, close } = await openWebview(demo('normal.csv'));
    try {
        await page.waitForSelector('.cell:not(.k-l)');
        await page.locator('#export-button').click();
        await page.locator('#export-start').click();
        const save = await takeSave();
        assert.doesNotMatch(await page.locator('#export-status').innerText(), /Saved/);
        assert.equal(await page.locator('#export-start').count(), 0);
        assert.equal(save.name, 'normal-export.csv');
        const text = Buffer.from(save.data).toString('utf8');
        const source = fs.readFileSync(demo('normal.csv'), 'utf8');
        assert.equal(text.split(/\r?\n/)[0], source.split(/\r?\n/)[0]);
        assert.equal(text.trim().split(/\r?\n/).length, source.trim().split(/\r?\n/).length);

        await completeSave(save.id, { saved: true });
        await page.locator('#export-status').filter({ hasText: 'Saved normal-export.csv' }).waitFor();

        assert.equal(await page.locator('#open-button').count(), 0);
    } finally {
        await close();
    }
});

test('cancelled and failed saves never show Saved and can be retried', async () => {
    const { page, takeSave, completeSave, close } = await openWebview(demo('normal.csv'));
    try {
        await page.waitForSelector('.cell:not(.k-l)');
        await page.locator('#export-button').click();
        await page.locator('#export-start').click();
        let save = await takeSave();
        await completeSave(save.id, { saved: false });
        await page.locator('#export-status').filter({ hasText: 'Save cancelled.' }).waitFor();

        await page.locator('#export-start').click();
        save = await takeSave();
        await completeSave(save.id, { saved: false, error: 'Cannot save: disk full' });
        await page.locator('#export-dialog .text-danger').filter({ hasText: 'disk full' }).waitFor();
        assert.doesNotMatch(await page.locator('#export-status').innerText(), /Saved/);

        await page.locator('#export-start').click();
        save = await takeSave();
        await completeSave(save.id, { saved: true });
        await page.locator('#export-status').filter({ hasText: 'Saved normal-export.csv' }).waitFor();
    } finally {
        await close();
    }
});

test('a late save result does not finish the export of a reopened dialog', async () => {
    const { page, takeSave, completeSave, close } = await openWebview(demo('normal.csv'));
    try {
        await page.waitForSelector('.cell:not(.k-l)');
        await page.locator('#export-button').click();
        await page.locator('#export-start').click();
        const previous = await takeSave();
        await page
            .locator('#export-dialog button')
            .filter({ hasText: /^Close$/ })
            .click();
        await page.locator('#export-button').click();
        await page.locator('#export-start').click();
        const current = await takeSave();
        assert.notEqual(previous.id, current.id);
        await completeSave(previous.id, { saved: true });
        assert.doesNotMatch(await page.locator('#export-status').innerText(), /Saved/);
        assert.equal(await page.locator('#export-start').count(), 0);
        await completeSave(current.id, { saved: true });
        await page.locator('#export-status').filter({ hasText: 'Saved normal-export.csv' }).waitFor();
    } finally {
        await close();
    }
});

test('chart image saves wait for the extension and show write failures', async () => {
    const { page, takeSave, completeSave, close } = await openWebview(demo('normal.csv'));
    try {
        await page.waitForSelector('.cell:not(.k-l)');
        await page.locator('#histogram-button').click();
        await page.locator('#histogram-chart .bar').first().waitFor();
        await page.locator('#save-svg').click();
        const save = await takeSave();
        assert.match(save.name, /\.svg$/);
        assert.match(Buffer.from(save.data).toString('utf8'), /^<svg/);
        assert.equal(await page.locator('#save-svg').isDisabled(), true);
        await completeSave(save.id, { saved: false, error: 'Cannot save: disk full' });
        await page.locator('#histogram-dialog .text-danger').filter({ hasText: 'disk full' }).waitFor();
        assert.equal(await page.locator('#save-svg').isEnabled(), true);
    } finally {
        await close();
    }
});
