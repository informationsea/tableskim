// TableSkim as a read-only custom editor: the web app (web/src/vscode/,
// built into media/) in a webview, and the engine in this extension host,
// which reads the document's file in place.

import { readFileSync, realpathSync } from 'node:fs';
import { stat } from 'node:fs/promises';
import path from 'node:path';
import * as vscode from 'vscode';
import { Engine } from './engine';

export const TABLE_VIEW_TYPE = 'tableskim.table';
export const TEXT_VIEW_TYPE = 'tableskim.text';

// Writes come in bursts: one question once the file has been quiet this long.
const CHANGE_SETTLE_MS = 300;

// What the webview posts (web/src/vscode/port.ts).
type FromWebview =
    | { type: 'hello'; page: string }
    | { type: 'engine'; message: { id: number; method: string; params?: Record<string, unknown> } }
    | { type: 'save'; page: string; id: number; name: string; data: Uint8Array }
    | { type: 'testResult'; id: number; text?: string; error?: string };

interface SaveResult {
    saved: boolean;
    error?: string;
}

/** A message relayed between a webview and its engine, for the integration tests. */
export interface Relayed {
    document: vscode.Uri;
    direction: 'toEngine' | 'fromEngine';
    message: unknown;
}

/** What the integration tests drive (extension test mode only; web/src/vscode/testHook.ts). */
export interface Testing {
    /** Answers the save dialogs instead of the user (undefined: show them again). */
    setSaveTarget(target: vscode.Uri | undefined): void;
    /** Answers whether to reload a changed file instead of the user (undefined: ask again). */
    setReloadAnswer(answer: ((document: vscode.Uri) => boolean) | undefined): void;
    /** Clicks an element of the document's webview, once it is there and enabled. */
    click(document: vscode.Uri, selector: string): Promise<void>;
    /** The text of an element of the document's webview ('' if there is none). */
    text(document: vscode.Uri, selector: string): Promise<string>;
}

interface Editor {
    document: vscode.Uri;
    panel: vscode.WebviewPanel;
    reload: () => void;
    post: (message: unknown) => void;
}

function escapeAttribute(text: string) {
    return text.replace(/&/g, '&amp;').replace(/"/g, '&quot;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}

function fileName(document: vscode.Uri) {
    return document.path.slice(document.path.lastIndexOf('/') + 1);
}

async function isSource(document: vscode.Uri, target: vscode.Uri): Promise<boolean> {
    if (document.toString() === target.toString()) return true;
    if (target.scheme !== 'file') return false;
    const [source, destination] = await Promise.all([
        stat(document.fsPath, { bigint: true }),
        stat(target.fsPath, { bigint: true }).catch((error: NodeJS.ErrnoException) => {
            if (error.code === 'ENOENT') return null;
            throw error;
        }),
    ]);
    // stat follows symlinks; device and inode also catch hard links.
    return destination !== null && source.dev === destination.dev && source.ino === destination.ino;
}

export class TableSkimEditorProvider implements vscode.CustomReadonlyEditorProvider {
    private readonly relayed = new vscode.EventEmitter<Relayed>();
    readonly onDidRelay = this.relayed.event;
    private readonly editors = new Set<Editor>();
    private readonly testMode: boolean;
    private saveTarget: vscode.Uri | undefined;
    private reloadAnswer: ((document: vscode.Uri) => boolean) | undefined;
    private nextTestId = 1;
    private readonly testReplies = new Map<number, (reply: { text?: string; error?: string }) => void>();

    constructor(private readonly context: vscode.ExtensionContext) {
        this.testMode = context.extensionMode === vscode.ExtensionMode.Test;
    }

    openCustomDocument(uri: vscode.Uri): vscode.CustomDocument {
        return { uri, dispose: () => {} };
    }

    resolveCustomEditor(document: vscode.CustomDocument, panel: vscode.WebviewPanel) {
        const media = vscode.Uri.joinPath(this.context.extensionUri, 'media');
        const modulePath = vscode.Uri.joinPath(this.context.extensionUri, 'engine', 'tableskim-wasm-node.js').fsPath;
        const name = fileName(document.uri);
        panel.webview.options = { enableScripts: true, localResourceRoots: [media] };

        let disposed = false;
        let loads = 0;
        let engine: Engine | null = null;
        let page: string | null = null;
        let refreshWatch = () => {};
        const disposables: vscode.Disposable[] = [];
        const post = (message: unknown) => {
            if (!disposed) void panel.webview.postMessage(message);
        };
        // Loads the page afresh; it says hello once it listens, and gets a new
        // engine then, which opens the file as it is now.
        const reload = () => {
            if (!disposed) {
                page = null;
                refreshWatch();
                panel.webview.html = this.html(panel.webview, media, document.uri, ++loads);
            }
        };
        const editor: Editor = { document: document.uri, panel, reload, post };
        this.editors.add(editor);

        // A page that loads again (Developer: Reload Webviews, a crashed
        // webview, reload()) gets an engine of its own: the old one has
        // answered requests the new page never made.
        const startEngine = () => {
            engine?.dispose();
            engine = null;
            // The engine reads files by their path on the machine this runs
            // on (the remote one with Remote Development).
            if (document.uri.scheme !== 'file') {
                post({ type: 'engineError', message: `TableSkim cannot read ${document.uri.scheme}: files` });
                return;
            }
            const current = new Engine(
                modulePath,
                (message) => {
                    if (engine !== current) return;
                    this.relayed.fire({ document: document.uri, direction: 'fromEngine', message });
                    post({ type: 'engine', message });
                },
                (text) => {
                    if (engine !== current) return;
                    post({ type: 'engineError', message: text });
                    void vscode.window
                        .showErrorMessage(`TableSkim stopped reading ${name}: ${text}`, 'Reload')
                        .then((choice) => {
                            if (choice === 'Reload' && engine === current) reload();
                        });
                },
            );
            engine = current;
        };

        if (document.uri.scheme === 'file')
            refreshWatch = this.watch(document.uri, () => disposed, reload, disposables);

        panel.onDidDispose(() => {
            disposed = true;
            engine?.dispose();
            engine = null;
            this.editors.delete(editor);
            for (const disposable of disposables) disposable.dispose();
        });

        panel.webview.onDidReceiveMessage((message: FromWebview) => {
            switch (message.type) {
                case 'hello':
                    page = message.page;
                    refreshWatch();
                    startEngine();
                    break;
                case 'engine': {
                    let request = message.message;
                    // Only the document is opened, whatever the page asks for.
                    if (request.method === 'open')
                        request = { ...request, params: { ...request.params, path: document.uri.fsPath } };
                    this.relayed.fire({ document: document.uri, direction: 'toEngine', message: request });
                    engine?.post(request);
                    break;
                }
                case 'save':
                    if (message.page !== page) break;
                    void this.save(document.uri, message.name, message.data).then((result) => {
                        if (page === message.page)
                            post({ type: 'saveResult', page: message.page, id: message.id, ...result });
                    });
                    break;
                case 'testResult':
                    this.testReplies.get(message.id)?.(message);
                    this.testReplies.delete(message.id);
                    break;
            }
        });

        reload();
    }

    /** Loads the active TableSkim editor's file again (the Reload command). */
    reloadActive() {
        for (const editor of this.editors) if (editor.panel.active) editor.reload();
    }

    /** For the integration tests; see Testing. */
    readonly testing: Testing = {
        setSaveTarget: (target) => {
            this.saveTarget = target;
        },
        setReloadAnswer: (answer) => {
            this.reloadAnswer = answer;
        },
        click: async (document, selector) => {
            await this.askWebview(document, 'click', selector);
        },
        text: (document, selector) => this.askWebview(document, 'text', selector),
    };

    private askWebview(document: vscode.Uri, action: 'click' | 'text', selector: string): Promise<string> {
        const editor = [...this.editors].find((each) => each.document.toString() === document.toString());
        if (!editor) return Promise.reject(new Error(`no TableSkim editor for ${document.toString()}`));
        const id = this.nextTestId++;
        return new Promise((resolve, reject) => {
            this.testReplies.set(id, (reply) =>
                reply.error !== undefined ? reject(new Error(reply.error)) : resolve(reply.text ?? ''),
            );
            editor.post({ type: 'test', id, action, selector });
        });
    }

    // Asks whether to read the document again once it changes on disk, as
    // the engine reads it in place: the rows shown would be stale, or torn.
    // The directory is watched rather than a glob of the file's name, which
    // may hold glob characters; a symbolic link's target is watched too.
    private watch(
        document: vscode.Uri,
        isDisposed: () => boolean,
        reload: () => void,
        disposables: vscode.Disposable[],
    ) {
        let files = new Set<string>();
        const directories = new Map<string, vscode.Disposable[]>();
        let timer: ReturnType<typeof setTimeout> | undefined;
        let asking = false;
        const ask = async () => {
            asking = true;
            try {
                const choice = this.reloadAnswer
                    ? this.reloadAnswer(document) && 'Reload'
                    : await vscode.window.showInformationMessage(
                          `${fileName(document)} has changed on disk. Reload it in TableSkim?`,
                          'Reload',
                      );
                if (choice === 'Reload' && !isDisposed()) reload();
            } finally {
                asking = false;
            }
        };
        const changed = (uri: vscode.Uri) => {
            if (asking || isDisposed() || !files.has(uri.fsPath)) return;
            clearTimeout(timer);
            timer = setTimeout(() => void ask(), CHANGE_SETTLE_MS);
        };
        const refresh = () => {
            clearTimeout(timer);
            files = new Set([document.fsPath]);
            try {
                files.add(realpathSync(document.fsPath));
            } catch {
                // Gone already: the engine will say so.
            }
            const needed = new Set([...files].map((file) => path.dirname(file)));
            for (const [directory, watched] of directories) {
                if (needed.has(directory)) continue;
                for (const disposable of watched) disposable.dispose();
                directories.delete(directory);
            }
            for (const directory of needed) {
                if (directories.has(directory)) continue;
                const watcher = vscode.workspace.createFileSystemWatcher(
                    new vscode.RelativePattern(vscode.Uri.file(directory), '*'),
                );
                directories.set(directory, [watcher, watcher.onDidChange(changed), watcher.onDidCreate(changed)]);
            }
        };
        disposables.push({
            dispose: () => {
                clearTimeout(timer);
                for (const watched of directories.values())
                    for (const disposable of watched) disposable.dispose();
                directories.clear();
            },
        });
        refresh();
        return refresh;
    }

    private html(webview: vscode.Webview, media: vscode.Uri, document: vscode.Uri, load: number) {
        const page = readFileSync(vscode.Uri.joinPath(media, 'vscode.html').fsPath, 'utf8');
        const source = webview.cspSource;
        const csp = [
            "default-src 'none'",
            `script-src ${source}`,
            // React sets style attributes; Tailwind's styles are a file.
            `style-src ${source} 'unsafe-inline'`,
            // Charts are drawn through images of their SVG for PNG.
            `img-src ${source} blob: data:`,
            `font-src ${source}`,
        ].join('; ');
        const head =
            `<meta http-equiv="Content-Security-Policy" content="${csp}">` +
            `<base href="${webview.asWebviewUri(media).toString()}/">` +
            `<meta name="tableskim-file" content="${escapeAttribute(fileName(document))}">` +
            // VS Code ignores html equal to the last; this makes each load differ.
            `<meta name="tableskim-load" content="${load}">` +
            (this.testMode ? '<meta name="tableskim-test" content="1">' : '');
        // Webview resources are not served for CORS requests.
        return page.replace('<head>', `<head>${head}`).replace(/ crossorigin/g, '');
    }

    private async save(document: vscode.Uri, name: string, data: Uint8Array): Promise<SaveResult> {
        let target: vscode.Uri | undefined;
        try {
            target =
                this.saveTarget ??
                (await vscode.window.showSaveDialog({
                    defaultUri: vscode.Uri.joinPath(document, '..', name),
                    title: `Save ${name}`,
                }));
            if (!target) return { saved: false };
            if (await isSource(document, target)) throw new Error('The file being viewed cannot be overwritten');
            await vscode.workspace.fs.writeFile(target, data instanceof Uint8Array ? data : new Uint8Array(data));
            return { saved: true };
        } catch (error) {
            return { saved: false, error: `Cannot save ${target?.fsPath ?? name}: ${String(error)}` };
        }
    }
}
