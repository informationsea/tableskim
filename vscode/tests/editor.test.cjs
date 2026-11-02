// The real provider's save messages, with VS Code's UI and engine replaced.
const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const Module = require('node:module');
const os = require('node:os');
const path = require('node:path');
const { test } = require('node:test');
const { pathToFileURL } = require('node:url');

class Uri {
    constructor(file) {
        this.fsPath = this.path = file;
        this.scheme = 'file';
    }
    static joinPath(base, ...parts) {
        return new Uri(path.join(base.fsPath, ...parts));
    }
    toString() {
        return pathToFileURL(this.fsPath).href;
    }
}

let chooseTarget;
let writeFile;
const vscode = {
    Uri,
    EventEmitter: class {
        event = () => ({ dispose() {} });
    },
    window: { showSaveDialog: (...args) => chooseTarget(...args) },
    workspace: { fs: { writeFile: (...args) => writeFile(...args) } },
};
const load = Module._load;
Module._load = function (name, ...args) {
    if (name === 'vscode') return vscode;
    if (name === './engine' && args[0]?.filename === require.resolve('../out/editor.js'))
        return {
            Engine: class {
                dispose() {}
            },
        };
    return load.call(this, name, ...args);
};
const { TableSkimEditorProvider } = require('../out/editor.js');
Module._load = load;

test('save replies report the result and protect the source', async (t) => {
    const directory = await fs.mkdtemp(path.join(os.tmpdir(), 'tableskim-save-'));
    t.after(() => fs.rm(directory, { recursive: true, force: true }));
    await fs.mkdir(path.join(directory, 'media'));
    await fs.writeFile(path.join(directory, 'media', 'vscode.html'), '<html><head></head></html>');
    const source = new Uri(path.join(directory, 'source.parquet'));
    await fs.writeFile(source.fsPath, 'original data');
    const data = Uint8Array.from([1, 2, 3, 255]);
    const replies = new Map();
    const pending = new Map();
    let receive;
    const provider = new TableSkimEditorProvider({ extensionUri: new Uri(directory) });
    provider.resolveCustomEditor(
        { uri: source },
        {
            onDidDispose() {},
            webview: {
                cspSource: 'https://webview.test',
                asWebviewUri: (uri) => uri,
                onDidReceiveMessage: (callback) => {
                    receive = callback;
                },
                postMessage: (message) => {
                    replies.set(message.id, message);
                    pending.get(message.id)?.(message);
                },
            },
        },
    );
    let id = 0;
    const save = () => {
        const request = ++id;
        return new Promise((resolve) => {
            pending.set(request, resolve);
            receive({ type: 'save', id: request, name: 'export.csv', data });
        });
    };
    t.beforeEach(() => {
        writeFile = (target, bytes) => fs.writeFile(target.fsPath, bytes);
    });

    await t.test('the document itself is rejected', async () => {
        chooseTarget = async () => source;
        const result = await save();
        assert.equal(result.saved, false);
        assert.match(result.error, /file being viewed cannot be overwritten/);
    });
    for (const kind of ['symlink', 'link']) {
        await t.test(`${kind} aliases are rejected`, async (t) => {
            const alias = new Uri(path.join(directory, `${kind}.parquet`));
            try {
                await fs[kind](source.fsPath, alias.fsPath);
            } catch (error) {
                if (error.code === 'EPERM') return t.skip('Link creation requires permission on this system');
                throw error;
            }
            chooseTarget = async () => alias;
            const result = await save();
            assert.equal(result.saved, false);
            assert.match(result.error, /file being viewed cannot be overwritten/);
        });
    }
    await t.test('new and existing destinations can be saved', async () => {
        const target = new Uri(path.join(directory, 'export.csv'));
        chooseTarget = async () => target;
        for (let attempt = 0; attempt < 2; attempt++) {
            assert.equal((await save()).saved, true);
            assert.deepEqual(await fs.readFile(target.fsPath), Buffer.from(data));
        }
    });
    await t.test('cancelling the dialog does not write anything', async () => {
        chooseTarget = async () => undefined;
        writeFile = () => assert.fail('cancelled saves must not write');
        const result = await save();
        assert.equal(result.saved, false);
        assert.equal(result.error, undefined);
    });
    await t.test('write and dialog failures are returned to the webview', async () => {
        chooseTarget = async () => new Uri(path.join(directory, 'export.csv'));
        writeFile = async () => {
            throw new Error('disk full');
        };
        let result = await save();
        assert.equal(result.saved, false);
        assert.match(result.error, /disk full/);
        chooseTarget = async () => {
            throw new Error('dialog failed');
        };
        result = await save();
        assert.equal(result.saved, false);
        assert.match(result.error, /dialog failed/);
    });
    await t.test('a success reply waits until writing finishes', async () => {
        chooseTarget = async () => new Uri(path.join(directory, 'export.csv'));
        let started, finish;
        const writing = new Promise((resolve) => {
            started = resolve;
        });
        writeFile = () => {
            started();
            return new Promise((resolve) => {
                finish = resolve;
            });
        };
        const result = save();
        await writing;
        assert.equal(replies.has(id), false);
        finish();
        assert.deepEqual(await result, { type: 'saveResult', id, saved: true });
    });
    assert.equal(await fs.readFile(source.fsPath, 'utf8'), 'original data');
});
