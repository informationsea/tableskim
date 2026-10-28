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
    browser = await chromium.launch({ executablePath: process.env.TABLESKIM_CHROMIUM });
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

// A deterministic worker for frontend regressions: SQL replies are held,
// and row replies enforce the same 500-column limit as WebSession.
async function regressionPage() {
    const context = await browser.newContext();
    await context.addInitScript(() => {
        window.Worker = class {
            constructor() {
                window.regressionWorker = this;
                this.requests = [];
                this.columns = 600;
                this.view = { sort: [], filter: null };
                this.sheet = 1;
                setTimeout(() => this.onmessage({ data: { type: 'ready' } }), 0);
            }
            reply(request, result) {
                queueMicrotask(() =>
                    this.onmessage({ data: { type: 'reply', json: JSON.stringify({ id: request.id, result }) } }),
                );
            }
            info() {
                return {
                    sheet: this.sheet,
                    name: this.name,
                    state: 'ready',
                    rows: 1,
                    rowsExact: true,
                    displayRows: 1,
                    rowsText: '1 rows',
                    hasHeader: true,
                    columns: Array.from({ length: this.columns }, (_, i) => ({
                        name: `c${i}`,
                        mark: '',
                        kind: 'text',
                        format: null,
                    })),
                    capabilities: { sort: true, filter: true, search: true },
                    view: this.view,
                    pending: false,
                    filterText: '',
                    ...(this.viewError ? { viewError: this.viewError, viewErrorId: this.viewErrorId } : {}),
                    checkpoint: null,
                    readingStopped: false,
                };
            }
            releaseSql() {
                this.reply(this.sql, { sheet: 'SQL Result', sheets: ['wide', 'narrow', 'SQL Result'] });
                this.sql = null;
                this.postMessage(this.waitingOpen);
            }
            postMessage(request) {
                this.requests.push(request);
                const { method, params } = request;
                if (method === 'filterOps')
                    return this.reply(request, [{ op: 'equals', label: 'Equals', takesValue: true }]);
                if (method === 'sql') {
                    this.sql = request;
                    return;
                }
                if (method === 'open') {
                    if (this.sql) {
                        this.waitingOpen = request;
                        return;
                    }
                    this.sheet++;
                    this.columns = params.file.name === 'second.csv' ? 2 : 600;
                    this.name = 'wide';
                    return this.reply(request, {
                        sheets: ['wide', 'narrow'],
                        supportsSql: true,
                        usesHeaderOption: false,
                        isTextFormat: false,
                        textEncoding: null,
                    });
                }
                if (method === 'sheet') {
                    this.name = params.name;
                    if (params.name === 'narrow') {
                        this.columns = 2;
                        this.sheet++;
                    }
                    return this.reply(request, this.info());
                }
                if (method === 'info') return this.reply(request, this.info());
                if (method === 'cellFilter')
                    return this.reply(request, {
                        conditions: [{ column: params.column, op: 'equals', value: `value-${params.column}` }],
                    });
                if (method === 'setView') {
                    let filter = 'filter' in params ? params.filter : this.view.filter;
                    if (params.addConditions)
                        filter = {
                            combine: filter?.combine ?? 'and',
                            conditions: [...(filter?.conditions ?? []), ...params.addConditions],
                        };
                    this.view = { sort: 'sort' in params ? params.sort : this.view.sort, filter };
                    if (this.failViews) {
                        this.viewError = 'the source failed';
                        this.viewErrorId = (this.viewErrorId ?? 0) + 1;
                    }
                    this.reply(request, { view: this.view });
                    queueMicrotask(() =>
                        this.onmessage({ data: { type: 'events', json: JSON.stringify({ [this.sheet]: 4 }) } }),
                    );
                    return;
                }
                if (method === 'rows') {
                    const count = Math.min(500, params.lastColumn - params.firstColumn + 1);
                    return this.reply(request, {
                        first: params.first,
                        firstColumn: params.firstColumn,
                        rows:
                            params.first === 0
                                ? [Array.from({ length: count }, (_, i) => `value-${params.firstColumn + i}`)]
                                : [],
                        kinds: params.first === 0 ? ['s'.repeat(count)] : [],
                        loading: false,
                        end: true,
                    });
                }
                throw new Error(`unexpected request: ${method}`);
            }
        };
    });
    const page = await context.newPage();
    await page.goto(baseUrl);
    await page.waitForSelector('body[data-ready=true]');
    await page.setInputFiles('#file-input', {
        name: 'first.sqlite',
        mimeType: 'application/octet-stream',
        buffer: Buffer.from('test'),
    });
    await waitForText(cell(page, 0, 0), 'value-0');
    return page;
}

describe('frontend regressions', () => {
    test('ignores SQL results from a file that was replaced', async () => {
        const page = await regressionPage();
        await page.locator('#sql-toggle').click();
        await page.fill('#sql-editor', 'SELECT 1');
        await page.locator('#sql-run').click();
        await page.waitForFunction(() => !!window.regressionWorker.sql);
        await page.setInputFiles('#file-input', {
            name: 'second.csv',
            mimeType: 'text/csv',
            buffer: Buffer.from('x\n1'),
        });
        await page.waitForFunction(() => !!window.regressionWorker.waitingOpen);
        await page.evaluate(() => window.regressionWorker.releaseSql());
        await waitForText(cell(page, 0, 0), 'value-0');
        await waitForText(page.locator('#status-rows'), '1 rows');
        assert.equal(
            await page.evaluate(() =>
                window.regressionWorker.requests.some((r) => r.method === 'sheet' && r.params.name === 'SQL Result'),
            ),
            false,
        );
        await page.context().close();
    });

    test('fetches columns past 500 while keeping a column frozen', async () => {
        const page = await regressionPage();
        await page.locator('.header[data-column="0"]').click({ button: 'right' });
        await page.locator('#context-menu').getByText('Freeze Up to This Column').click();
        await cell(page, 0, 0).click();
        await page.keyboard.press('Control+ArrowRight');
        await waitForText(cell(page, 0, 599), 'value-599');
        await waitForText(cell(page, 0, 0), 'value-0');
        assert.equal(
            await page.evaluate(() =>
                window.regressionWorker.requests
                    .filter((r) => r.method === 'rows')
                    .every((r) => r.params.lastColumn - r.params.firstColumn < 500),
            ),
            true,
        );
        // The frozen column is fetched on its own, not with every column
        // up to the ones in view.
        assert.equal(
            await page.evaluate(() =>
                window.regressionWorker.requests
                    .filter((r) => r.method === 'rows')
                    .every((r) => r.params.firstColumn > 0 || r.params.lastColumn < 100),
            ),
            true,
        );
        await page.context().close();
    });

    test('changes only its part of the view and reports a failed view once', async () => {
        const page = await regressionPage();
        await page.locator('.header[data-column="1"]').click({ button: 'right' });
        await page.locator('#context-menu').getByText('Sort Ascending').click();
        await page.waitForFunction(() => window.regressionWorker.view.sort.length === 1);
        await page.evaluate(() => (window.regressionWorker.failViews = true));
        await cell(page, 0, 1).click({ button: 'right' });
        await page.locator('#context-menu').getByText('Filter: Equal to This Value').click();
        await page.waitForFunction(() => !!window.regressionWorker.view.filter);
        const setViews = await page.evaluate(() =>
            window.regressionWorker.requests.filter((r) => r.method === 'setView').map((r) => r.params),
        );
        assert.ok(!('filter' in setViews[0]), 'a sort leaves the filter to the worker');
        assert.ok(!('sort' in setViews[1]), 'a filter leaves the sort to the worker');
        assert.equal(setViews[1].addConditions.length, 1);
        assert.equal((await page.evaluate(() => window.regressionWorker.view)).sort.length, 1);
        await waitForText(page.locator('#message'), 'The sort or filter failed: the source failed');
        await page.waitForFunction(() => {
            const [header, view] = [
                document.querySelector('.header[data-column="1"] .name'),
                window.regressionWorker.view,
            ];
            return view.filter && header?.classList.contains('name') && header.classList.contains('text-accent');
        });
        await page.context().close();
    });

    test('filters the displayed column after switching to a narrower sheet', async () => {
        const page = await regressionPage();
        await page.locator('#filter-toggle').click();
        await page.selectOption('#filter-column', '599');
        await page.locator('#tabs button', { hasText: 'narrow' }).click();
        await page.waitForFunction(() => document.querySelectorAll('#filter-column option').length === 2);
        assert.equal(await page.locator('#filter-column').inputValue(), '1');
        await page.fill('#filter-value', 'value-1');
        await page.locator('#filter-form button[type=submit]').click();
        await page.waitForFunction(() => !!window.regressionWorker.view.filter);
        assert.equal(await page.evaluate(() => window.regressionWorker.view.filter.conditions[0].column), 1);
        await page.context().close();
    });
});

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

    test('follows the cursor with statistics and filters by a cell', async () => {
        const page = await newPage();
        await open(page, demo('normal.csv'));
        await cell(page, 0, 3).click();
        await page.locator('#stats-toggle').click();
        const stat = (label) => page.locator('#stats-table tr', { hasText: label }).locator('td');
        await waitForText(stat('Rows'), '54');
        await waitForText(stat('Min'), '20');
        await cell(page, 0, 0).click();
        await waitForText(page.locator('#stats-panel h2'), 'ID');
        await waitForText(stat('Max'), '54');

        // Osaka, from a cell's menu.
        await cell(page, 0, 4).click({ button: 'right' });
        await page.locator('#context-menu').getByText('Filter: Equal to This Value').click();
        await waitForText(page.locator('#status-rows'), '4 rows of 54');
        await waitForText(stat('Rows'), '4');
        await page.context().close();
    });

    test('filters by the bars of a histogram', async () => {
        const page = await newPage();
        await open(page, demo('normal.csv'));
        await cell(page, 0, 4).click();
        await page.locator('#histogram-button').click();
        // City is text: its most frequent values, Fukuoka first.
        await waitForText(page.locator('#histogram-summary'), 'Click a bar');
        await page.locator('#histogram-chart .bar').first().click();
        await waitForText(page.locator('#status-rows'), '9 rows of 54');

        await page.selectOption('#histogram-column', '3');
        await waitForText(page.locator('#histogram-summary'), '9 numbers');
        await page.locator('#histogram-table').waitFor({ state: 'detached' });
        await page.locator('#histogram-dialog button', { hasText: 'Table' }).click();
        const rows = page.locator('#histogram-table tbody tr');
        await rows.first().waitFor();
        let total = 0;
        for (const count of await rows.locator('td:last-child').allTextContents()) total += Number(count);
        assert.equal(total, 9);
        await page.keyboard.press('Escape');
        await page.locator('#histogram-dialog').waitFor({ state: 'detached' });
        await page.context().close();
    });

    test('plots a scatter plot and filters by a rectangle drawn on it', async () => {
        const page = await newPage();
        await open(page, demo('normal.csv'));
        await cell(page, 0, 3).click();
        await page.locator('#scatter-button').click();
        await waitForText(page.locator('#scatter-summary'), '53 rows plotted');
        // The regression line is drawn by default and can be turned off.
        await page.locator('#scatter-chart[data-fit=true]').waitFor();
        await page.locator('#scatter-fit').uncheck();
        await page.locator('#scatter-chart:not([data-fit])').waitFor();
        await page.locator('#scatter-fit').check();
        await page.locator('#scatter-density').click();
        await page.locator('#scatter-chart[data-fit=true]').waitFor();
        // Around the whole plot: every row plotted is kept.
        const box = await page.locator('#scatter-chart').boundingBox();
        await page.mouse.move(box.x + 66, box.y + 14);
        await page.mouse.down();
        await page.mouse.move(box.x + box.width / 2, box.y + 200, { steps: 4 });
        await page.mouse.move(box.x + box.width - 18, box.y + box.height - 46, { steps: 4 });
        await page.mouse.up();
        await waitForText(page.locator('#status-rows'), '53 rows of 54');

        // A text X: box plots of each city.
        await page.selectOption('#scatter-x', '4');
        await page.locator('#scatter-boxes:not([disabled])').click();
        await waitForText(page.locator('#scatter-summary'), 'Click a box');
        assert.equal(await page.locator('#scatter-fit').isDisabled(), true, 'no line for categories');
        await page.context().close();
    });

    test('hides, freezes and formats columns and goes to a cell', async () => {
        const page = await newPage();
        await open(page, demo('normal.csv'));
        await page.locator('.header[data-column="2"]').click({ button: 'right' });
        await page.locator('#context-menu').getByText('Hide Column').click();
        await page.locator('.header[data-column="2"]').waitFor({ state: 'detached' });
        await waitForText(page.locator('#status-rows'), '1 column hidden');

        await page.locator('.header[data-column="1"]').click({ button: 'right' });
        await page.locator('#context-menu').getByText('Freeze Up to This Column').click();
        await page.locator('.header.frozen[data-column="1"]').waitFor();

        await page.locator('.header[data-column="6"]').click({ button: 'right' });
        await page.locator('#context-menu').getByText('Format…').click();
        await page.check('#format-thousands');
        await page.selectOption('#format-decimals', '0');
        await page.locator('#format-apply').click();
        await waitForText(cell(page, 0, 6), '941,752');

        // Copies keep the values, without hidden columns.
        await cell(page, 0, 1).click();
        await cell(page, 0, 6).click({ modifiers: ['Shift'] });
        await page.keyboard.press('Control+c');
        await waitForText(page.locator('#status-cell'), 'Copied 1 rows');
        assert.equal(
            await page.evaluate(() => navigator.clipboard.readText()),
            'Sakura Brown\t45\tOsaka\tEngineering\t941751.65\n',
        );

        await page.keyboard.press('Control+g');
        await page.fill('#go-to-text', 'E30');
        await page.keyboard.press('Enter');
        await page.waitForSelector('.cell.cursor[data-row="29"][data-column="4"]');
        await page.context().close();
    });

    test('exports the view and groups it into a sheet', async () => {
        const page = await newPage();
        await open(page, demo('normal.csv'));
        await page.locator('.header[data-column="2"]').click({ button: 'right' });
        await page.locator('#context-menu').getByText('Hide Column').click();
        await page.locator('#export-button').click();
        const download = page.waitForEvent('download');
        await page.locator('#export-start').click();
        const file = await download;
        assert.equal(file.suggestedFilename(), 'normal-export.csv');
        const text = require('node:fs').readFileSync(await file.path(), 'utf8');
        assert.match(text, /^ID,Name,Age,City,/);
        await waitForText(page.locator('#export-status'), 'Saved normal-export.csv');
        await waitForText(page.locator('#export-status'), '54 rows');
        await page.keyboard.press('Escape');

        await cell(page, 0, 4).click();
        await page.locator('#group-by-button').click();
        await page.locator('.group-value').nth(2).check(); // Age: Email is hidden
        await page.locator('#group-by-run').click();
        await page.waitForFunction(
            () => document.querySelector('#tabs [aria-selected=true]')?.textContent === 'Group By',
        );
        await waitForText(page.locator('#status-rows'), '12 rows');
        await waitForText(page.locator('.header .name'), 'Age mean');
        await page.context().close();
    });

    test('shows a sheet whose opening was reported before its info', async () => {
        const context = await browser.newContext();
        // Replies that a sheet is opening reach the page late, after the
        // event that it opened: the page must still show the sheet.
        await context.addInitScript(() => {
            const Base = window.Worker;
            window.Worker = class extends Base {
                set onmessage(handler) {
                    super.onmessage = (event) => {
                        const late = event.data.type === 'reply' && event.data.json.includes('"state":"opening"');
                        if (late) setTimeout(() => handler(event), 500);
                        else handler(event);
                    };
                }
            };
        });
        const page = await context.newPage();
        await page.goto(baseUrl);
        await page.waitForSelector('body[data-ready=true]');
        await open(page, demo('normal.csv'));
        await cell(page, 0, 4).click();
        await page.locator('#group-by-button').click();
        await page.locator('#group-by-run').click();
        await page.waitForFunction(
            () => document.querySelector('#tabs [aria-selected=true]')?.textContent === 'Group By',
        );
        await waitForText(page.locator('#status-rows'), '12 rows');
        await page.waitForSelector('.cell:not(.k-l)');
        await context.close();
    });

    test('runs SQL with completion and history', async () => {
        const page = await newPage();
        await open(page, demo('demo.sqlite3'));
        await page.locator('#sql-toggle').click();
        await page.fill('#sql-editor', 'SELECT City, count(*) AS n FROM nor');
        await page.press('#sql-editor', 'End');
        await page.press('#sql-editor', 'Tab');
        await page.waitForFunction(() => document.querySelector('#sql-editor').value.endsWith('FROM normal'));
        await page.locator('#sql-editor').pressSequentially(' GROUP BY City ORDER BY n DESC, City');
        await page.press('#sql-editor', 'Control+Enter');
        await waitForText(page.locator('#sql-status'), 'SQL Result');
        await page.waitForFunction(
            () => document.querySelector('#tabs [aria-selected=true]')?.textContent === 'SQL Result',
        );
        await waitForText(cell(page, 0, 0), 'Fukuoka');
        await waitForText(cell(page, 0, 1), '9');

        await page.fill('#sql-editor', 'SELECT nonsense FROM nowhere');
        await page.locator('#sql-run').click();
        await page.locator('#sql-status.text-danger').waitFor();
        await page.press('#sql-editor', 'Control+ArrowUp');
        await page.waitForFunction(() => document.querySelector('#sql-editor').value.startsWith('SELECT nonsense'));
        await page.press('#sql-editor', 'Control+ArrowUp');
        await page.waitForFunction(() => document.querySelector('#sql-editor').value.startsWith('SELECT City'));
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
