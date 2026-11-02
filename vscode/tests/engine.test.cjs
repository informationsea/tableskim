// The engine in Node.js, as the extension runs it: opens demo files by
// their paths and reads rows. Needs `npm run build:engine` and `npm run compile`.
const assert = require('node:assert');
const path = require('node:path');
const { test } = require('node:test');
const { Engine } = require('../out/engine.js');

const demo = path.resolve(__dirname, '../../demo/data');
const modulePath = path.resolve(__dirname, '../engine/tableskim-wasm-node.js');

// A client of the protocol (web/src/client.ts, in short).
function start() {
    const pending = new Map();
    const events = [];
    let nextId = 1;
    let markReady;
    const ready = new Promise((resolve) => (markReady = resolve));
    let failure = null;
    const engine = new Engine(
        modulePath,
        (message) => {
            if (message.type === 'ready') markReady();
            else if (message.type === 'reply') {
                const reply = JSON.parse(message.json);
                const request = pending.get(reply.id);
                pending.delete(reply.id);
                if (reply.error !== undefined) request.reject(new Error(reply.error));
                else request.resolve(reply.result);
            } else if (message.type === 'file') {
                pending.get(message.id).resolve(message.data);
                pending.delete(message.id);
            } else if (message.type === 'events') events.push(JSON.parse(message.json));
        },
        (text) => (failure = text),
    );
    const call = (method, params = {}) =>
        new Promise((resolve, reject) => {
            const id = nextId++;
            pending.set(id, { resolve, reject });
            engine.post({ id, method, params });
        });
    return { engine, ready, call, events, failure: () => failure };
}

// Asks until the answer passes, as the page asks again on events.
async function until(ask, passes, timeout = 30000) {
    const end = Date.now() + timeout;
    for (;;) {
        const answer = await ask();
        if (passes(answer)) return answer;
        if (Date.now() > end) throw new Error(`timed out: ${JSON.stringify(answer).slice(0, 500)}`);
        await new Promise((resolve) => setTimeout(resolve, 50));
    }
}

async function firstRows(client, file, sheetName) {
    const book = await client.call('open', { path: path.join(demo, file) });
    const sheet = await client.call('sheet', { name: sheetName ?? book.sheets[0] });
    const info = await until(
        () => client.call('info', { sheet: sheet.sheet }),
        (info) => info.state !== 'opening' && info.rowsExact,
    );
    assert.strictEqual(info.state, 'ready', info.error);
    const rows = await until(
        () => client.call('rows', { sheet: sheet.sheet, first: 0, last: 2, firstColumn: 0, lastColumn: 3 }),
        (rows) => !rows.loading,
    );
    return { book, info, rows };
}

test('the engine opens files by their paths', async (t) => {
    const client = start();
    t.after(() => client.engine.dispose());
    await client.ready;

    await t.test('CSV', async () => {
        const { book, info, rows } = await firstRows(client, 'normal.csv');
        assert.ok(book.isTextFormat);
        assert.ok(info.rows > 0);
        assert.ok(info.columns.length > 0);
        assert.strictEqual(rows.rows.length, 3);
    });

    await t.test('Parquet, with SQL', async () => {
        const { book } = await firstRows(client, 'normal.parquet');
        assert.ok(book.supportsSql);
    });

    await t.test('Excel', async () => {
        const { book } = await firstRows(client, 'multisheet.xlsx');
        assert.ok(book.sheets.length > 1);
    });

    await t.test('a file name that is not ASCII', async () => {
        const { book } = await firstRows(client, '多言語サンプル.csv');
        assert.ok(book.sheets.length > 0);
    });

    await t.test('a missing file fails', async () => {
        await assert.rejects(client.call('open', { path: path.join(demo, 'no-such-file.csv') }));
    });

    await t.test('an export is read back', async () => {
        const book = await client.call('open', { path: path.join(demo, 'normal.csv') });
        const sheet = await client.call('sheet', { name: book.sheets[0] });
        await until(
            () => client.call('info', { sheet: sheet.sheet }),
            (info) => info.state === 'ready' && info.rowsExact,
        );
        await client.call('startExport', { sheet: sheet.sheet, format: 'csv', hidden: [] });
        const job = await until(
            () => client.call('jobState', {}),
            (state) => state.done,
        );
        assert.ok(!job.error, job.error);
        const data = await client.call('readFile', { path: job.file });
        assert.ok(data.byteLength > 0);
    });

    assert.strictEqual(client.failure(), null);
});
