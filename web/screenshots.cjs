// Takes the manual's screenshots of the web app (docs/images/web-*.png)
// from the app as it is, in headless Chromium (Playwright, a package of
// web/), at the size the manual uses:
//   LANG=C.UTF-8 node web/screenshots.cjs [build dir [output dir]]
// The build directory holds the module and the built app, as for
// serve.mjs (default build/wasm-release, or TABLESKIM_WASM_BUILD); the
// output directory defaults to docs/images. Look at each image before
// committing it.
const { spawn } = require('node:child_process');
const path = require('node:path');
const { chromium } = require('playwright');

const root = path.join(__dirname, '..');
const buildDir = path.resolve(root, process.argv[2] ?? process.env.TABLESKIM_WASM_BUILD ?? 'build/wasm-release');
const outDir = path.resolve(root, process.argv[3] ?? 'docs/images');
const demo = (name) => path.join(root, 'demo', 'data', name);
const VIEWPORT = { width: 1244, height: 746 };

const cell = (page, row, column) => page.locator(`.cell[data-row="${row}"][data-column="${column}"]`);

// Starts serve.mjs; resolves with the server and its URL.
async function startServer() {
    const port = String(9000 + Math.floor(Math.random() * 900));
    const child = spawn(process.execPath, [path.join(__dirname, 'serve.mjs'), buildDir, port], {
        stdio: ['ignore', 'pipe', 'inherit'],
    });
    await new Promise((resolve) => child.stdout.once('data', resolve));
    return { child, url: `http://127.0.0.1:${port}/` };
}

// The views shown in the manual, each taken from a page of its own.
const SHOTS = {
    // The main view: the Content pane with the cursor's cell.
    'web-main.png': async (open) => {
        const page = await open('normal.csv');
        await cell(page, 2, 2).dblclick();
        await page.locator('#sidebar').waitFor();
        return page;
    },
    // The Search pane, a hit selected.
    'web-search-pane.png': async (open) => {
        const page = await open('normal.csv');
        await page.keyboard.press('Control+f');
        await page.locator('#search-text').waitFor();
        await page.keyboard.type('watanabe');
        await page.keyboard.press('Enter');
        await page.locator('#search-status').filter({ hasText: '20 hits' }).waitFor();
        await page.locator('.search-hit').nth(3).filter({ hasText: 'Taro' }).waitFor();
        await page.locator('.search-hit').nth(3).click();
        return page;
    },
    // The Filter pane with two conditions: City = Tokyo and Age ≥ 45.
    'web-filter-pane.png': async (open) => {
        const page = await open('normal.csv');
        await cell(page, 2, 2).click();
        await page.locator('#filter-toggle').click();
        for (const [index, column, op, value] of [
            [0, '4', '=', 'Tokyo'],
            [1, '3', '≥', '45'],
        ]) {
            await page.locator('#filter-add').click();
            await page.locator('.filter-column').nth(index).selectOption(column);
            await page.locator('.filter-op').nth(index).selectOption({ label: op });
            await page.locator('.filter-value').nth(index).fill(value);
        }
        await page.locator('#filter-apply').click();
        await page.locator('#status-rows').filter({ hasText: '4 rows of 54' }).waitFor();
        await cell(page, 2, 2).click();
        return page;
    },
    // A histogram of Salary in 30 bins.
    'web-histogram.png': async (open) => {
        const page = await open('normal.csv');
        await cell(page, 0, 6).click();
        await page.locator('#histogram-button').click();
        await page.locator('#histogram-dialog').waitFor();
        await page.fill('#histogram-bins', '30');
        await page.press('#histogram-bins', 'Enter');
        await page.locator('#histogram-summary').filter({ hasText: '30 bins' }).waitFor();
        await page.locator('#histogram-bins').blur();
        return page;
    },
    // SQL on a DuckDB file, its result shown as a sheet.
    'web-sql.png': async (open) => {
        const page = await open('demo.duckdb');
        await page.locator('#sql-toggle').click();
        await page
            .locator('#sql-editor')
            .fill(
                'SELECT City, count(*) AS people, round(avg(Salary)) AS salary\n' +
                    'FROM normal\nGROUP BY City\nORDER BY people DESC, City',
            );
        await page.locator('#sql-run').click();
        await page.locator('#tabs button[aria-selected=true]').filter({ hasText: 'SQL Result' }).waitFor();
        await page.locator('#status-rows').filter({ hasText: '12 rows' }).waitFor();
        await page.waitForSelector('.cell:not(.k-l)');
        await cell(page, 0, 0).click();
        await page.locator('#status-cell').filter({ hasText: 'City' }).waitFor();
        return page;
    },
};

async function main() {
    const only = process.env.TABLESKIM_SCREENSHOTS?.split(',');
    const { child, url } = await startServer();
    const browser = await chromium.launch({ executablePath: process.env.TABLESKIM_CHROMIUM });
    try {
        for (const [name, take] of Object.entries(SHOTS)) {
            if (only && !only.includes(name)) continue;
            const context = await browser.newContext({ viewport: VIEWPORT, colorScheme: 'light' });
            const open = async (file) => {
                const page = await context.newPage();
                page.on('pageerror', (error) => console.error(`${name}: page error: ${error.message}`));
                await page.goto(url);
                await page.waitForSelector('body[data-ready=true]');
                await page.setInputFiles('#file-input', demo(file));
                await page.waitForSelector('.cell:not(.k-l)');
                await page.locator('#status-rows').filter({ hasText: 'rows' }).waitFor();
                return page;
            };
            const page = await take(open);
            // Transitions and the last repaint: what the page shows is waited
            // for above, this only lets it finish drawing.
            await page.waitForTimeout(400);
            await page.screenshot({ path: path.join(outDir, name) });
            console.log(`wrote ${path.join(outDir, name)}`);
            await context.close();
        }
    } finally {
        await browser.close();
        child.kill();
    }
}

main().catch((error) => {
    console.error(error);
    process.exit(1);
});
