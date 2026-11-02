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
        this.fsPath = file;
        // As VS Code's: slashes, and a drive letter after one on Windows.
        this.path = path.sep === '\\' ? `/${file.replace(/\\/g, '/')}` : file;
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
let answerInformation = async () => undefined;
let answerError = async () => undefined;
const shownInformation = [];
const shownErrors = [];
const watchers = [];
const engines = [];
const vscode = {
    Uri: Object.assign(Uri, { file: (file) => new Uri(file) }),
    EventEmitter: class {
        event = () => ({ dispose() {} });
        fire() {}
    },
    ExtensionMode: { Production: 1, Development: 2, Test: 3 },
    RelativePattern: class {
        constructor(base, pattern) {
            this.baseUri = base;
            this.pattern = pattern;
        }
    },
    window: {
        showSaveDialog: (...args) => chooseTarget(...args),
        showInformationMessage: (...args) => {
            shownInformation.push(args);
            return answerInformation(...args);
        },
        showErrorMessage: (...args) => {
            shownErrors.push(args);
            return answerError(...args);
        },
    },
    workspace: {
        fs: { writeFile: (...args) => writeFile(...args) },
        createFileSystemWatcher: (pattern) => {
            const listeners = { change: [], create: [] };
            const watcher = {
                pattern,
                disposed: false,
                fire: (kind, uri) => listeners[kind].forEach((listener) => listener(uri)),
                onDidChange: (listener) => (listeners.change.push(listener), { dispose() {} }),
                onDidCreate: (listener) => (listeners.create.push(listener), { dispose() {} }),
                dispose: () => (watcher.disposed = true),
            };
            watchers.push(watcher);
            return watcher;
        },
    },
};
const load = Module._load;
Module._load = function (name, ...args) {
    if (name === 'vscode') return vscode;
    if (name === './engine' && args[0]?.filename === require.resolve('../out/editor.js'))
        return {
            // Records what it is asked; the tests post and fail for it.
            Engine: class {
                constructor(modulePath, onMessage, onError) {
                    Object.assign(this, { onMessage, onError, posted: [], disposed: false });
                    engines.push(this);
                }
                post(message) {
                    this.posted.push(message);
                }
                dispose() {
                    this.disposed = true;
                }
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
            onDidChangeViewState() {},
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

// A provider with an editor on a document, as VS Code resolves it.
// With linked, the document is a symbolic link to a file in another directory.
async function resolveEditor(t, { linked = false } = {}) {
    // Without links (macOS's temporary directory is under /var -> /private/var),
    // so that the document's directory is the only one watched.
    const directory = await fs.realpath(await fs.mkdtemp(path.join(os.tmpdir(), 'tableskim-editor-')));
    t.after(() => fs.rm(directory, { recursive: true, force: true }));
    await fs.mkdir(path.join(directory, 'media'));
    await fs.writeFile(path.join(directory, 'media', 'vscode.html'), '<html><head></head></html>');
    const document = new Uri(path.join(directory, 'data.csv'));
    if (linked) {
        await fs.mkdir(path.join(directory, 'target'));
        await fs.writeFile(path.join(directory, 'target', 'real.csv'), 'a,b\n1,2\n');
        await fs.symlink(path.join(directory, 'target', 'real.csv'), document.fsPath);
    } else await fs.writeFile(document.fsPath, 'a,b\n1,2\n');
    engines.length = watchers.length = shownInformation.length = shownErrors.length = 0;
    const posted = [];
    const loads = [];
    let receive;
    let dispose;
    const panel = {
        active: true,
        onDidDispose: (callback) => {
            dispose = callback;
        },
        webview: {
            cspSource: 'https://webview.test',
            asWebviewUri: (uri) => uri,
            set html(value) {
                loads.push(value);
            },
            onDidReceiveMessage: (callback) => {
                receive = callback;
            },
            postMessage: async (message) => void posted.push(message),
        },
    };
    const provider = new TableSkimEditorProvider({ extensionUri: new Uri(directory), extensionMode: 1 });
    provider.resolveCustomEditor({ uri: document }, panel);
    return {
        provider,
        document,
        panel,
        posted,
        loads,
        receive: (message) => receive(message),
        dispose: () => dispose(),
    };
}

const until = async (condition) => {
    for (let i = 0; i < 200 && !condition(); i++) await new Promise((resolve) => setTimeout(resolve, 10));
    assert.ok(condition());
};

test('each load of the page gets an engine of its own', async (t) => {
    const editor = await resolveEditor(t);
    assert.equal(editor.loads.length, 1);
    assert.doesNotMatch(editor.loads[0], /tableskim-test/);
    assert.equal(engines.length, 0, 'no engine before the page listens');
    editor.receive({ type: 'hello' });
    assert.equal(engines.length, 1);
    editor.receive({ type: 'engine', message: { id: 1, method: 'open', params: { path: '/elsewhere' } } });
    assert.equal(engines[0].posted[0].params.path, editor.document.fsPath);

    // The page loaded again: the old engine stops, and nothing it still posts reaches the page.
    editor.receive({ type: 'hello' });
    assert.equal(engines.length, 2);
    assert.equal(engines[0].disposed, true);
    engines[0].onMessage({ type: 'ready' });
    engines[0].onError('late failure');
    engines[1].onMessage({ type: 'ready' });
    assert.deepEqual(editor.posted, [{ type: 'engine', message: { type: 'ready' } }]);
    assert.equal(shownErrors.length, 0);

    // The Reload command loads the page again, with html that differs from the last.
    editor.provider.reloadActive();
    assert.equal(editor.loads.length, 2);
    assert.notEqual(editor.loads[1], editor.loads[0]);
    editor.dispose();
    assert.equal(engines[1].disposed, true);
    assert.ok(watchers.every((watcher) => watcher.disposed));
    editor.provider.reloadActive();
    assert.equal(editor.loads.length, 2, 'a closed editor is not loaded');
});

test('a stopped engine offers to reload the page', async (t) => {
    const editor = await resolveEditor(t);
    editor.receive({ type: 'hello' });
    let choose;
    answerError = () => new Promise((resolve) => (choose = resolve));
    engines[0].onError('the engine exited (1)');
    assert.deepEqual(editor.posted, [{ type: 'engineError', message: 'the engine exited (1)' }]);
    assert.match(shownErrors[0][0], /stopped reading data\.csv: the engine exited \(1\)/);
    choose('Reload');
    await until(() => editor.loads.length === 2);
});

test('a change on disk asks whether to reload, once per burst', async (t) => {
    const editor = await resolveEditor(t);
    editor.receive({ type: 'hello' });
    assert.equal(watchers.length, 1);
    assert.equal(watchers[0].pattern.baseUri.fsPath, path.dirname(editor.document.fsPath));
    let choose;
    answerInformation = () => new Promise((resolve) => (choose = resolve));

    // Other files of the directory are not the document.
    watchers[0].fire('change', new Uri(path.join(path.dirname(editor.document.fsPath), 'other.csv')));
    for (let i = 0; i < 5; i++) watchers[0].fire('change', editor.document);
    await until(() => shownInformation.length === 1);
    assert.match(shownInformation[0][0], /data\.csv has changed on disk\. Reload it in TableSkim\?/);
    // Not asked again while the question is open.
    watchers[0].fire('create', editor.document);
    await new Promise((resolve) => setTimeout(resolve, 500));
    assert.equal(shownInformation.length, 1);
    choose('Reload');
    await until(() => editor.loads.length === 2);

    // Dismissed: the page stays as it is, and a later change asks again.
    answerInformation = async () => undefined;
    watchers[0].fire('change', editor.document);
    await until(() => shownInformation.length === 2);
    await new Promise((resolve) => setTimeout(resolve, 50));
    assert.equal(editor.loads.length, 2);

    // A change pending when the editor closes asks nothing.
    watchers[0].fire('change', editor.document);
    editor.dispose();
    await new Promise((resolve) => setTimeout(resolve, 500));
    assert.equal(shownInformation.length, 2);
});

test('a linked document is watched where it points too', async (t) => {
    let editor;
    try {
        editor = await resolveEditor(t, { linked: true });
    } catch (error) {
        if (error.code === 'EPERM') return t.skip('Link creation requires permission on this system');
        throw error;
    }
    const directory = path.dirname(editor.document.fsPath);
    assert.deepEqual(
        watchers.map((watcher) => watcher.pattern.baseUri.fsPath),
        [directory, path.join(directory, 'target')],
    );
    answerInformation = async () => 'Reload';
    watchers[1].fire('change', new Uri(path.join(directory, 'target', 'real.csv')));
    await until(() => editor.loads.length === 2);
    editor.dispose();
});
