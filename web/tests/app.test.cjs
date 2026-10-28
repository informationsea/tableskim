// End-to-end tests of the web app in headless Chromium (Playwright):
//   TABLESKIM_WASM_BUILD=build/wasm-release node --test web/tests/
// Run with a UTF-8 locale (LANG=C.UTF-8): Chromium drops non-ASCII paths
// given to a file input otherwise.
const { after, before, describe, test } = require('node:test');
const assert = require('node:assert/strict');
const { spawn } = require('node:child_process');
const path = require('node:path');
const { chromium } = require('playwright');

const root = path.join(__dirname, '..', '..');
const buildDir = path.resolve(root, process.env.TABLESKIM_WASM_BUILD ?? 'build/wasm-release');
const demo = (name) => path.join(root, 'demo', 'data', name);

let server;
let browser;
let baseUrl;

// Starts web/serve.mjs; resolves with the server and its URL.
async function startServer(...options) {
    const port = String(9000 + Math.floor(Math.random() * 900));
    const child = spawn(process.execPath, [path.join(root, 'web', 'serve.mjs'), ...options, buildDir, port], {
        stdio: ['ignore', 'pipe', 'inherit'],
    });
    await new Promise((resolve) => child.stdout.once('data', resolve));
    return { child, url: `http://127.0.0.1:${port}/` };
}

before(async () => {
    ({ child: server, url: baseUrl } = await startServer());
    browser = await chromium.launch();
});

after(async () => {
    await browser?.close();
    server?.kill();
});

async function newPage() {
    const context = await browser.newContext();
    await context.grantPermissions(['clipboard-read', 'clipboard-write'], { origin: baseUrl });
    const page = await context.newPage();
    page.on('pageerror', (error) => console.error('page error:', error.message));
    await page.goto(baseUrl);
    await page.waitForSelector('body[data-ready=true]');
    return page;
}

async function open(page, file) {
    await page.setInputFiles('#file-input', file);
    await page.waitForSelector('.cell:not(.k-l)');
}

const cell = (page, row, column) => page.locator(`.cell[data-row="${row}"][data-column="${column}"]`);

async function waitForText(locator, text) {
    await locator.filter({ hasText: text }).first().waitFor();
}

describe('the web app', () => {
    test('shows a CSV file and its row count', async () => {
        const page = await newPage();
        await open(page, demo('normal.csv'));
        await waitForText(cell(page, 0, 1), 'Sakura Brown');
        await waitForText(page.locator('#status-rows'), '54 rows');
        assert.match(await page.locator('#status-file').textContent(), /normal\.csv \(UTF-8\)/);
        assert.equal(await page.locator('.header[data-column="0"] .name').textContent(), 'ID');
        assert.ok((await cell(page, 0, 0).getAttribute('class')).includes('k-n'), 'numbers are aligned right');
        await page.context().close();
    });

    test('sorts by a header click and filters', async () => {
        const page = await newPage();
        await open(page, demo('normal.csv'));
        await page.locator('.header[data-column="0"]').click();
        await waitForText(page.locator('.header[data-column="0"] .sort'), '▲');
        await page.locator('.header[data-column="0"]').click();
        await waitForText(page.locator('.header[data-column="0"] .sort'), '▼');
        await waitForText(cell(page, 0, 0), '54');

        await page.locator('#filter-toggle').click();
        await page.selectOption('#filter-column', { label: 'City' });
        await page.selectOption('#filter-op', 'equals');
        await page.fill('#filter-value', 'Osaka');
        await page.locator('#filter-form button[type=submit]').click();
        await waitForText(page.locator('#status-rows'), '4 rows of 54');
        await waitForText(page.locator('#status-filter'), 'City = "Osaka"');
        for (let row = 0; row < 4; row++) await waitForText(cell(page, row, 4), 'Osaka');
        assert.equal(await cell(page, 4, 4).count(), 0, 'no rows past the filtered ones');
        // Still sorted by ID, descending.
        const ids = [];
        for (let row = 0; row < 4; row++) ids.push(Number(await cell(page, row, 0).textContent()));
        assert.deepEqual(
            [...ids].sort((a, b) => b - a),
            ids,
        );

        await page.locator('#filter-conditions button').click();
        await waitForText(page.locator('#status-rows'), '54 rows');
        await page.context().close();
    });

    test('reports an invalid regular expression in a filter', async () => {
        const page = await newPage();
        await open(page, demo('normal.csv'));
        await page.locator('#filter-toggle').click();
        await page.selectOption('#filter-op', 'matches');
        await page.fill('#filter-value', '(');
        await page.locator('#filter-form button[type=submit]').click();
        await page.locator('#message:not([hidden])').waitFor();
        await page.context().close();
    });

    test('searches and moves between hits', async () => {
        const page = await newPage();
        await open(page, demo('normal.csv'));
        await page.fill('#search-text', 'tokyo');
        await page.press('#search-text', 'Enter');
        await waitForText(page.locator('#search-status'), '7 hits');
        await waitForText(page.locator('.cell.cursor'), 'Tokyo');
        const first = await page.locator('.cell.cursor').getAttribute('data-row');
        await page.press('#search-text', 'Enter');
        await page.waitForFunction((row) => document.querySelector('.cell.cursor')?.dataset.row !== row, first);
        await waitForText(page.locator('.cell.cursor'), 'Tokyo');
        await page.press('#search-text', 'Shift+Enter');
        await page.waitForFunction((row) => document.querySelector('.cell.cursor')?.dataset.row === row, first);
        await page.context().close();
    });

    test('copies the selected cells', async () => {
        const page = await newPage();
        await open(page, demo('normal.csv'));
        await cell(page, 0, 0).click();
        await cell(page, 1, 1).click({ modifiers: ['Shift'] });
        await page.keyboard.press('Control+c');
        await waitForText(page.locator('#status-cell'), 'Copied 2 rows');
        assert.equal(await page.evaluate(() => navigator.clipboard.readText()), '1\tSakura Brown\n2\tHanako Suzuki\n');
        await page.context().close();
    });

    test('switches between the sheets of a database', async () => {
        const page = await newPage();
        await open(page, demo('demo.sqlite3'));
        const tabs = page.locator('#tabs button');
        assert.ok((await tabs.count()) > 1);
        const second = await tabs.nth(1).textContent();
        await tabs.nth(1).click();
        await page.waitForFunction(
            (name) => document.querySelector('#tabs [aria-selected=true]')?.textContent === name,
            second,
        );
        await page.waitForSelector('.cell:not(.k-l)');
        await page.context().close();
    });

    test('reads the first row as data on request', async () => {
        const page = await newPage();
        await open(page, demo('normal.csv'));
        await page.selectOption('#header-mode', 'no');
        await waitForText(cell(page, 0, 1), 'Name');
        await waitForText(page.locator('#status-rows'), '55 rows');
        await page.context().close();
    });

    test('goes to the end of a larger file with the keyboard', async () => {
        const page = await newPage();
        await open(page, demo('large.tsv.gz'));
        // A text file is read as far as needed: the end is not known yet.
        await waitForText(page.locator('#status-rows'), '≥');
        await cell(page, 0, 0).click();
        await page.keyboard.press('Control+End');
        await waitForText(page.locator('#status-rows'), '10000 rows');
        await waitForText(page.locator('.row-number[data-row="9999"]'), '10000');
        await waitForText(cell(page, 9999, 0), '10000');
        await page.context().close();
    });

    test('opens the last of files given in quick succession', async () => {
        const page = await newPage();
        // The second file is mounted before the first is answered, which
        // must not take the second one's mount away.
        await page.setInputFiles('#file-input', demo('normal.xlsx'));
        await page.setInputFiles('#file-input', demo('多言語サンプル.csv'));
        await waitForText(page.locator('.cell'), '日本語');
        assert.match(await page.locator('#status-file').textContent(), /多言語サンプル\.csv/);
        assert.equal(await page.locator('#message').isHidden(), true);
        await page.context().close();
    });

    test('opens Parquet, Excel and JSON files and non-ASCII names', async () => {
        const page = await newPage();
        for (const [file, text] of [
            ['normal.parquet', 'Sakura Brown'],
            ['normal.xlsx', 'Sakura Brown'],
            ['normal.jsonl.gz', 'Sakura Brown'],
            ['多言語サンプル.csv', '日本語'],
        ]) {
            await open(page, demo(file));
            await waitForText(page.locator('.cell'), text);
            assert.match(await page.locator('#status-file').textContent(), new RegExp(file.replace('.', '\\.')));
        }
        await page.context().close();
    });
});

// GitHub Pages cannot send the headers that make the page cross-origin
// isolated: the app's service worker adds them, after one reload.
describe('the web app on a host without the isolation headers', () => {
    let pagesServer;
    let pagesUrl;

    before(async () => {
        ({ child: pagesServer, url: pagesUrl } = await startServer('--no-isolation-headers'));
    });

    after(() => pagesServer?.kill());

    test('is isolated by its service worker and opens a file', async () => {
        const context = await browser.newContext();
        const page = await context.newPage();
        page.on('pageerror', (error) => console.error('page error:', error.message));
        const response = await page.goto(pagesUrl);
        assert.equal(response.headers()['cross-origin-embedder-policy'], undefined, 'the server sends no headers');
        await page.waitForSelector('body[data-ready=true]');
        assert.equal(await page.evaluate(() => self.crossOriginIsolated), true);
        assert.equal(await page.evaluate(() => navigator.serviceWorker.controller !== null), true);
        // Parquet runs DuckDB, whose threads are workers too.
        await open(page, demo('normal.parquet'));
        await waitForText(cell(page, 0, 1), 'Sakura Brown');
        await context.close();
    });

    test('is not taken over by the service worker where the server isolates it', async () => {
        const page = await newPage();
        assert.equal(await page.evaluate(() => self.crossOriginIsolated), true);
        assert.equal(await page.evaluate(async () => (await navigator.serviceWorker.getRegistrations()).length), 0);
        await page.context().close();
    });
});
