// Takes the manual's screenshots of the web app (docs/images/web-*.png)
// from the app as it is, in headless Chromium (Playwright, a package of
// web/), at the size the manual uses:
//   LANG=C.UTF-8 node web/screenshots.cjs [build dir [output dir]]
// The build directory holds the module and the built app, as for
// serve.mjs (default build/wasm-release, or TABLESKIM_WASM_BUILD); the
// output directory defaults to docs/images. Look at each image before
// committing it.
const fs = require('node:fs');
const { spawn } = require('node:child_process');
const path = require('node:path');
const { chromium } = require('playwright');

const root = path.join(__dirname, '..');
const buildDir = path.resolve(root, process.argv[2] ?? process.env.TABLESKIM_WASM_BUILD ?? 'build/wasm-release');
const outDir = path.resolve(root, process.argv[3] ?? 'docs/images');
const demo = (name) => path.join(root, 'demo', 'examples', name);
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
        const page = await open('iris.csv');
        await cell(page, 2, 4).dblclick();
        await page.locator('#sidebar').waitFor();
        return page;
    },
    // The Search pane, a hit selected.
    'web-search-pane.png': async (open) => {
        const page = await open('iris.csv');
        await page.keyboard.press('Control+f');
        await page.locator('#search-text').waitFor();
        await page.keyboard.type('setosa');
        await page.keyboard.press('Enter');
        await page.locator('#search-status').filter({ hasText: '50 hits' }).waitFor();
        await page.locator('.search-hit').nth(3).filter({ hasText: 'setosa' }).waitFor();
        await page.locator('.search-hit').nth(3).click();
        return page;
    },
    // The Filter pane: virginica flowers with petals at least 6 cm long.
    'web-filter-pane.png': async (open) => {
        const page = await open('iris.csv');
        await cell(page, 2, 2).click();
        await page.locator('#filter-toggle').click();
        for (const [index, column, op, value] of [
            [0, '4', '=', 'virginica'],
            [1, '2', '≥', '6'],
        ]) {
            await page.locator('#filter-add').click();
            await page.locator('.filter-column').nth(index).selectOption(column);
            await page.locator('.filter-op').nth(index).selectOption({ label: op });
            await page.locator('.filter-value').nth(index).fill(value);
        }
        await page.locator('#filter-apply').click();
        await page.locator('#status-rows').filter({ hasText: '11 rows of 150' }).waitFor();
        await cell(page, 2, 2).click();
        return page;
    },
    // A histogram of Iris petal length in 30 bins.
    'web-histogram.png': async (open) => {
        const page = await open('iris.csv');
        await cell(page, 0, 2).click();
        await page.locator('#histogram-button').click();
        await page.locator('#histogram-dialog').waitFor();
        await page.fill('#histogram-bins', '30');
        await page.press('#histogram-bins', 'Enter');
        await page.locator('#histogram-summary').filter({ hasText: '30 bins' }).waitFor();
        await page.locator('#histogram-bins').blur();
        return page;
    },
    // Iris petal length stacked by species.
    'web-histogram-stacked.png': async (open) => {
        const page = await open('iris.csv');
        await cell(page, 0, 2).click();
        await page.locator('#histogram-button').click();
        await page.locator('#histogram-summary').filter({ hasText: '150 numbers' }).waitFor();
        await page.selectOption('#histogram-group', '4');
        await page.locator('#histogram-legend').filter({ hasText: 'setosa' }).waitFor();
        return page;
    },
    // Iris petal length against width, coloured by species.
    'web-scatter-colored.png': async (open) => {
        const page = await open('iris.csv');
        await cell(page, 0, 2).click();
        await page.locator('#scatter-button').click();
        await page.locator('#scatter-summary').filter({ hasText: '150 rows plotted' }).waitFor();
        await page.selectOption('#scatter-x', '2');
        await page.selectOption('#scatter-y', '3');
        await page.selectOption('#scatter-color', '4');
        await page.locator('#scatter-summary').filter({ hasText: 'click a colour' }).waitFor();
        return page;
    },
    // Convert the Iris CSV to Parquet as the manual explains, then run SQL.
    'web-sql.png': async (open) => {
        const page = await open('iris.csv');
        await page.locator('#convert-button').click();
        const [download] = await Promise.all([page.waitForEvent('download'), page.locator('#convert-start').click()]);
        const data = fs.readFileSync(await download.path());
        await page.locator('#convert-dialog').getByRole('button', { name: 'Close', exact: true }).last().click();
        await page.setInputFiles('#file-input', {
            name: 'iris.parquet',
            mimeType: 'application/octet-stream',
            buffer: data,
        });
        await page.locator('#status-rows').filter({ hasText: '150 rows' }).waitFor();
        await page.locator('#sql-toggle').click();
        await page
            .locator('#sql-editor')
            .fill(
                'SELECT species, count(*) AS flowers,\n' +
                    '       round(avg(petal_length), 3) AS mean_petal_length\n' +
                    'FROM iris\nGROUP BY species\nORDER BY species',
            );
        await page.locator('#sql-run').click();
        await page.locator('#tabs button[aria-selected=true]').filter({ hasText: 'SQL Result' }).waitFor();
        await page.locator('#status-rows').filter({ hasText: '3 rows' }).waitFor();
        await page.waitForSelector('.cell:not(.k-l)');
        await cell(page, 0, 0).click();
        await page.locator('#status-cell').filter({ hasText: 'species' }).waitFor();
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
