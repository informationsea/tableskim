// Runs the extension in VS Code (downloaded by @vscode/test-electron into
// .vscode-test/) with suite.cjs as its tests. Needs `npm run build` first;
// on Linux without a display, run it under xvfb-run:
//   xvfb-run -a node tests/integration/run.cjs
const os = require('node:os');
const fs = require('node:fs');
const path = require('node:path');
const { runTests } = require('@vscode/test-electron');

async function main() {
    const extensionDevelopmentPath = path.resolve(__dirname, '../..');
    // A profile of its own, so that the user's settings and extensions stay out.
    const userData = fs.mkdtempSync(path.join(os.tmpdir(), 'tableskim-vscode-'));
    try {
        await runTests({
            version: process.env.TABLESKIM_VSCODE_VERSION ?? 'stable',
            extensionDevelopmentPath,
            extensionTestsPath: path.join(__dirname, 'suite.cjs'),
            launchArgs: [
                path.resolve(extensionDevelopmentPath, '../demo/data'),
                '--disable-extensions',
                '--disable-workspace-trust',
                '--disable-gpu',
                '--no-sandbox',
                `--user-data-dir=${userData}`,
            ],
        });
    } finally {
        fs.rmSync(userData, { recursive: true, force: true });
    }
}

main().catch((error) => {
    console.error(error);
    process.exit(1);
});
