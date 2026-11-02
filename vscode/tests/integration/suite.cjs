// Runs in VS Code's extension host (run.cjs): opens demo files in the
// custom editor and waits until the webview has asked the engine for rows
// and got them, which needs the page loaded under its CSP, the engine
// started in the extension host and the document opened by its path.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const vscode = require('vscode');

const demo = path.resolve(__dirname, '../../../demo/data');
const TIMEOUT = 60000;

// Resolves once a "rows" request of the document's webview is answered with
// rows that are no longer loading; rejects if its "open" fails.
function waitForRows(api, uri) {
    return new Promise((resolve, reject) => {
        const rowsRequests = new Set();
        let openId = null;
        const timer = setTimeout(() => {
            subscription.dispose();
            reject(new Error(`no rows for ${uri.fsPath} in ${TIMEOUT} ms`));
        }, TIMEOUT);
        const finish = (callback) => {
            clearTimeout(timer);
            subscription.dispose();
            callback();
        };
        const subscription = api.onDidRelay(({ document, direction, message }) => {
            if (document.toString() !== uri.toString()) return;
            if (direction === 'toEngine') {
                if (message.method === 'rows') rowsRequests.add(message.id);
                if (message.method === 'open') openId = message.id;
                return;
            }
            if (message.type !== 'reply') return;
            const reply = JSON.parse(message.json);
            if (reply.id === openId && reply.error !== undefined)
                finish(() => reject(new Error(`cannot open ${uri.fsPath}: ${reply.error}`)));
            else if (
                rowsRequests.has(reply.id) &&
                reply.result &&
                !reply.result.loading &&
                reply.result.rows.length > 0
            )
                finish(() => resolve(reply.result));
        });
    });
}

async function openIn(api, file, viewType) {
    const uri = vscode.Uri.file(path.join(demo, file));
    const rows = waitForRows(api, uri);
    await vscode.commands.executeCommand('vscode.openWith', uri, viewType);
    const result = await rows;
    const input = vscode.window.tabGroups.activeTabGroup.activeTab?.input;
    assert.ok(input instanceof vscode.TabInputCustom, 'the active tab is a custom editor');
    assert.equal(input.viewType, viewType);
    await vscode.commands.executeCommand('workbench.action.closeActiveEditor');
    return result;
}

async function run() {
    const extension = vscode.extensions.getExtension('informationsea.tableskim');
    assert.ok(extension, 'the extension is installed');
    const api = await extension.activate();

    const cases = [
        ['normal.parquet', 'tableskim.table'],
        ['normal.xlsx', 'tableskim.table'],
        ['demo.sqlite3', 'tableskim.table'],
        ['demo.duckdb', 'tableskim.table'],
        ['normal.csv', 'tableskim.text'],
        ['normal.csv.gz', 'tableskim.text'],
        ['normal.ndjson', 'tableskim.text'],
        ['多言語サンプル.csv', 'tableskim.text'],
    ];
    for (const [file, viewType] of cases) {
        const result = await openIn(api, file, viewType);
        console.log(`ok ${file} (${viewType}): first row ${JSON.stringify(result.rows[0]).slice(0, 80)}`);
    }

    // Parquet opens in TableSkim by default (priority "default").
    const parquet = vscode.Uri.file(path.join(demo, 'dates.parquet'));
    const rows = waitForRows(api, parquet);
    await vscode.commands.executeCommand('vscode.open', parquet);
    await rows;
    console.log('ok dates.parquet opened by default');
    await vscode.commands.executeCommand('workbench.action.closeAllEditors');

    await reloads(api);
    await saves(api);
    await changes(api);
}

// A file changed on disk is read again once the user says so; here one
// outside the workspace, which VS Code watches only on request.
async function changes(api) {
    const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'tableskim-change-'));
    const file = path.join(directory, 'changing.csv');
    fs.writeFileSync(file, 'name,value\nfirst,1\n');
    const uri = vscode.Uri.file(file);
    const asked = [];
    try {
        api.testing.setReloadAnswer((document) => (asked.push(document.fsPath), true));
        let rows = waitForRows(api, uri);
        await vscode.commands.executeCommand('vscode.openWith', uri, 'tableskim.text');
        await rows;
        await until(
            () => api.testing.text(uri, '#status-rows'),
            (text) => /\b1 row\b/.test(text),
            'one row',
        );
        // VS Code may start watching a little after the editor opens.
        await new Promise((resolve) => setTimeout(resolve, 1000));
        rows = waitForRows(api, uri);
        fs.appendFileSync(file, 'second,2\n');
        await rows;
        await until(
            () => api.testing.text(uri, '#status-rows'),
            (text) => /\b2 rows\b/.test(text),
            'two rows',
        );
        // As VS Code names it (on Windows, with a lower-case drive letter).
        assert.deepEqual(asked, [uri.fsPath]);
        console.log('ok a file changed on disk is read again');
    } finally {
        api.testing.setReloadAnswer(undefined);
        await vscode.commands.executeCommand('workbench.action.closeAllEditors');
        removeLater(directory);
    }
}

// Removes a temporary directory, with retries: on Windows a file stays
// busy until the closed editor's engine has stopped. A failure to clean up
// is only reported, so that it never hides the test's own error.
function removeLater(directory) {
    try {
        fs.rmSync(directory, { recursive: true, force: true, maxRetries: 20, retryDelay: 250 });
    } catch (error) {
        console.warn(`cannot remove ${directory}: ${error.message}`);
    }
}

// Asks until the answer passes, as a user looks again.
async function until(ask, passes, what) {
    const end = Date.now() + TIMEOUT;
    for (;;) {
        const answer = await ask();
        if (passes(answer)) return answer;
        if (Date.now() > end) throw new Error(`timed out waiting for ${what}: last ${JSON.stringify(answer)}`);
        await new Promise((resolve) => setTimeout(resolve, 100));
    }
}

// A page loaded again, by VS Code or the Reload command, reads its document
// again with an engine of its own.
async function reloads(api) {
    const uri = vscode.Uri.file(path.join(demo, 'normal.parquet'));
    let rows = waitForRows(api, uri);
    await vscode.commands.executeCommand('vscode.openWith', uri, 'tableskim.table');
    await rows;
    rows = waitForRows(api, uri);
    await vscode.commands.executeCommand('workbench.action.webview.reloadWebviewAction');
    await rows;
    console.log('ok the page reloaded by Developer: Reload Webviews shows rows again');
    rows = waitForRows(api, uri);
    await vscode.commands.executeCommand('tableskim.reload');
    await rows;
    console.log('ok the Reload command shows rows again');
    await vscode.commands.executeCommand('workbench.action.closeAllEditors');
}

// An export crosses VS Code twice: the engine's file to the webview, and
// the bytes to save back to the extension, which writes them.
async function saves(api) {
    const source = path.join(demo, 'normal.csv');
    const uri = vscode.Uri.file(source);
    const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'tableskim-save-'));
    const target = path.join(directory, 'export.csv');
    try {
        const rows = waitForRows(api, uri);
        await vscode.commands.executeCommand('vscode.openWith', uri, 'tableskim.text');
        await rows;
        api.testing.setSaveTarget(vscode.Uri.file(target));
        await api.testing.click(uri, '#export-button');
        await api.testing.click(uri, '#export-start');
        const status = await until(
            () => api.testing.text(uri, '#export-status'),
            (text) => /Saved|Cannot/.test(text),
            'the export',
        );
        assert.match(status, /^Saved normal-export\.csv/);
        // The whole view, as the source has it.
        const lines = (text) => text.trimEnd().split(/\r?\n/);
        assert.equal(lines(fs.readFileSync(target, 'utf8')).length, lines(fs.readFileSync(source, 'utf8')).length);
        assert.equal(lines(fs.readFileSync(target, 'utf8'))[0], lines(fs.readFileSync(source, 'utf8'))[0]);
        console.log('ok an export is saved through the extension');
    } finally {
        api.testing.setSaveTarget(undefined);
        await vscode.commands.executeCommand('workbench.action.closeAllEditors');
        removeLater(directory);
    }
}

module.exports = { run };
