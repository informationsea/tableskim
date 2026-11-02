// Takes the README's screenshots of the extension (images/vscode-*.png) in
// VS Code itself, the version the integration test uses (downloaded by
// @vscode/test-electron into .vscode-test/), driven by Playwright (a
// package of web/). Needs `npm run build` first; on Linux without a
// display, run it under Xvfb at the screenshots' size:
//   xvfb-run -a -s "-screen 0 1280x800x24" node scripts/screenshots.cjs
// TABLESKIM_SCREENSHOTS=vscode-main.png,... takes only those. Look at each
// image before committing it.
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { downloadAndUnzipVSCode } = require('@vscode/test-electron');

const extensionDir = path.resolve(__dirname, '..');
const root = path.resolve(extensionDir, '..');
const { _electron: electron } = require(require.resolve('playwright', { paths: [path.join(root, 'web')] }));
const demo = path.join(root, 'demo', 'data');
const outDir = path.join(extensionDir, 'images');
// The window's title bar names the Extension Development Host: left out.
const TITLE_BAR = 35;

// The page in the active editor's webview.
const webview = (page) => page.frameLocator('iframe.webview.ready').frameLocator('#active-frame');
const cell = (frame, row, column) => frame.locator(`.cell[data-row="${row}"][data-column="${column}"]`);

// What each screenshot shows: the theme and the file, then what is done in the page.
const SHOTS = {
    // A Parquet file, opened from the Explorer, its cell in the sidebar.
    'vscode-main.png': {
        theme: 'Default Light Modern',
        file: 'normal.parquet',
        take: async (frame) => {
            await cell(frame, 2, 2).dblclick();
            await frame.locator('#sidebar').waitFor();
        },
    },
    // A histogram of Salary, in a dark theme the page follows.
    'vscode-histogram.png': {
        theme: 'Default Dark Modern',
        file: 'normal.parquet',
        take: async (frame) => {
            await cell(frame, 0, 6).click();
            await frame.locator('#histogram-button').click();
            await frame.locator('#histogram-dialog').waitFor();
            await frame.locator('#histogram-bins').fill('30');
            await frame.locator('#histogram-bins').press('Enter');
            await frame.locator('#histogram-summary').filter({ hasText: '30 bins' }).waitFor();
            await frame.locator('#histogram-bins').blur();
        },
    },
    // SQL on a DuckDB file, its result shown as a sheet.
    'vscode-sql.png': {
        theme: 'Default Light Modern',
        file: 'demo.duckdb',
        take: async (frame) => {
            await frame.locator('#sql-toggle').click();
            await frame
                .locator('#sql-editor')
                .fill(
                    'SELECT City, count(*) AS people, round(avg(Salary)) AS salary\n' +
                        'FROM normal\nGROUP BY City\nORDER BY people DESC, City',
                );
            await frame.locator('#sql-run').click();
            await frame.locator('#tabs button[aria-selected=true]').filter({ hasText: 'SQL Result' }).waitFor();
            await frame.locator('#status-rows').filter({ hasText: '12 rows' }).waitFor();
            await cell(frame, 0, 0).click();
            await frame.locator('#status-cell').filter({ hasText: 'City' }).waitFor();
        },
    },
};

async function shoot(code, name, { theme, file, take }) {
    // A profile of its own: the theme, and none of VS Code's prompts.
    const userData = fs.mkdtempSync(path.join(os.tmpdir(), 'tableskim-screenshots-'));
    fs.mkdirSync(path.join(userData, 'User'));
    fs.writeFileSync(
        path.join(userData, 'User', 'settings.json'),
        JSON.stringify({
            'workbench.colorTheme': theme,
            'workbench.startupEditor': 'none',
            'workbench.tips.enabled': false,
            'workbench.secondarySideBar.defaultVisibility': 'hidden',
            'chat.commandCenter.enabled': false,
            'extensions.ignoreRecommendations': true,
            'telemetry.telemetryLevel': 'off',
            'update.mode': 'none',
        }),
    );
    const app = await electron.launch({
        executablePath: code,
        args: [
            `--extensionDevelopmentPath=${extensionDir}`,
            '--disable-extensions',
            '--disable-workspace-trust',
            '--no-sandbox',
            `--user-data-dir=${userData}`,
            demo,
        ],
    });
    try {
        const page = await app.firstWindow();
        await page.locator('.monaco-workbench').waitFor();
        // The first start greets with a sign-in dialog, a while after the
        // workbench shows; Quick Open waits for it to be gone.
        await page
            .getByText('Continue without Signing In')
            .click({ timeout: 60000 })
            .catch(() => console.warn('no sign-in dialog'));
        await page.waitForTimeout(1000);
        await page.keyboard.press('Escape');
        // Opened as from the Explorer: Quick Open, the file's name.
        // Pressed again while the workbench still takes the focus elsewhere.
        const quickOpen = page.locator('.quick-input-widget input');
        for (let attempt = 0; !(await quickOpen.isVisible()); attempt++) {
            if (attempt === 20) throw new Error('Quick Open does not open');
            await page.keyboard.press('Escape');
            await page.keyboard.press('Control+P');
            await page.waitForTimeout(1000);
        }
        await page.keyboard.type(file);
        await page.locator('.quick-input-list .monaco-list-row', { hasText: file }).first().waitFor();
        await page.keyboard.press('Enter');
        const frame = webview(page);
        await frame.locator('.cell:not(.k-l)').first().waitFor({ timeout: 60000 });
        await frame.locator('#status-rows').filter({ hasText: 'rows' }).waitFor();
        await take(frame);
        // Transitions and the last repaint: what the page shows is waited
        // for above, this only lets it finish drawing.
        await page.waitForTimeout(500);
        const size = page.viewportSize() ?? (await page.evaluate(() => ({ width: innerWidth, height: innerHeight })));
        await page.screenshot({
            path: path.join(outDir, name),
            clip: { x: 0, y: TITLE_BAR, width: size.width, height: size.height - TITLE_BAR },
        });
        console.log(`wrote ${path.join(outDir, name)}`);
    } finally {
        await app.close();
        fs.rmSync(userData, { recursive: true, force: true, maxRetries: 10, retryDelay: 200 });
    }
}

async function main() {
    const only = process.env.TABLESKIM_SCREENSHOTS?.split(',');
    const code = await downloadAndUnzipVSCode({
        version: process.env.TABLESKIM_VSCODE_VERSION ?? 'stable',
        cachePath: path.join(extensionDir, '.vscode-test'),
    });
    fs.mkdirSync(outDir, { recursive: true });
    for (const [name, shot] of Object.entries(SHOTS)) if (!only || only.includes(name)) await shoot(code, name, shot);
}

main().catch((error) => {
    console.error(error);
    process.exit(1);
});
