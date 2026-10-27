// Runs the probe page in headless Chromium (Playwright) on one file:
//   node web/probe/run-browser.cjs <build dir> <file> [SQL]
const { spawn } = require('node:child_process');
const path = require('node:path');
const { chromium } = require('playwright');

(async () => {
    const [buildDir, file, sql] = process.argv.slice(2);
    if (!buildDir || !file) {
        console.error('usage: node run-browser.cjs <build dir> <file> [SQL]');
        process.exit(2);
    }
    const port = String(8765 + Math.floor(Math.random() * 1000));
    const server = spawn(process.execPath, [path.join(__dirname, '..', 'serve.mjs'), buildDir, port], {
        stdio: ['ignore', 'pipe', 'inherit'],
    });
    await new Promise((resolve) => server.stdout.once('data', resolve));
    const browser = await chromium.launch();
    let code;
    try {
        const page = await browser.newPage();
        page.on('console', (message) => console.error('console:', message.text()));
        page.on('pageerror', (error) => console.error('page error:', error.message));
        await page.goto(`http://127.0.0.1:${port}/probe/`);
        if (!(await page.evaluate(() => self.crossOriginIsolated)))
            throw new Error('the page is not cross-origin isolated');
        await page.setInputFiles('#file', file);
        // Chromium drops a non-ASCII path without a UTF-8 locale.
        if (!(await page.evaluate(() => document.getElementById('file').files.length)))
            throw new Error('the file was not selected (run with LANG=C.UTF-8 for a non-ASCII name)');
        if (sql) await page.fill('#sql', sql);
        const start = Date.now();
        await page.click('#run');
        try {
            await page.waitForFunction(() => document.getElementById('status').textContent.startsWith('exit'), null, {
                timeout: Number(process.env.PROBE_TIMEOUT_MS ?? 600000),
            });
        } finally {
            console.log((await page.textContent('#log')).trimEnd());
        }
        const status = await page.textContent('#status');
        console.log(
            `browser: ${status}, ${((Date.now() - start) / 1000).toFixed(3)} s from the click (loading the module included)`,
        );
        code = status === 'exit 0' ? 0 : 1;
    } finally {
        await browser.close();
        server.kill();
    }
    process.exit(code);
})().catch((error) => {
    console.error(error);
    process.exit(1);
});
