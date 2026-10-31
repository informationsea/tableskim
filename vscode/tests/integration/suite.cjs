// Runs in VS Code's extension host (run.cjs): opens demo files in the
// custom editor and waits until the webview has asked the engine for rows
// and got them, which needs the page loaded under its CSP, the engine
// started in the extension host and the document opened by its path.
const assert = require('node:assert/strict');
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
}

module.exports = { run };
