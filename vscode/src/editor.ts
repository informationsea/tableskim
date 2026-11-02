// TableSkim as a read-only custom editor: the web app (web/src/vscode/,
// built into media/) in a webview, and the engine in this extension host,
// which reads the document's file in place.

import { readFileSync } from 'node:fs';
import { stat } from 'node:fs/promises';
import * as vscode from 'vscode';
import { Engine } from './engine';

export const TABLE_VIEW_TYPE = 'tableskim.table';
export const TEXT_VIEW_TYPE = 'tableskim.text';

// What the webview posts (web/src/vscode/port.ts).
type FromWebview =
    | { type: 'engine'; message: { id: number; method: string; params?: Record<string, unknown> } }
    | { type: 'save'; id: number; name: string; data: Uint8Array };

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

function escapeAttribute(text: string) {
    return text.replace(/&/g, '&amp;').replace(/"/g, '&quot;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
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

    constructor(private readonly context: vscode.ExtensionContext) {}

    openCustomDocument(uri: vscode.Uri): vscode.CustomDocument {
        return { uri, dispose: () => {} };
    }

    resolveCustomEditor(document: vscode.CustomDocument, panel: vscode.WebviewPanel) {
        const media = vscode.Uri.joinPath(this.context.extensionUri, 'media');
        panel.webview.options = { enableScripts: true, localResourceRoots: [media] };
        panel.webview.html = this.html(panel.webview, media, document.uri);

        const post = (message: unknown) => void panel.webview.postMessage(message);
        // The engine reads files by their path on the machine this runs on
        // (the remote one with Remote Development).
        if (document.uri.scheme !== 'file') {
            post({ type: 'engineError', message: `TableSkim cannot read ${document.uri.scheme}: files` });
            return;
        }
        const modulePath = vscode.Uri.joinPath(this.context.extensionUri, 'engine', 'tableskim-wasm-node.js').fsPath;
        const engine = new Engine(
            modulePath,
            (message) => {
                this.relayed.fire({ document: document.uri, direction: 'fromEngine', message });
                post({ type: 'engine', message });
            },
            (text) => post({ type: 'engineError', message: text }),
        );
        panel.onDidDispose(() => engine.dispose());

        panel.webview.onDidReceiveMessage((message: FromWebview) => {
            switch (message.type) {
                case 'engine': {
                    let request = message.message;
                    // Only the document is opened, whatever the page asks for.
                    if (request.method === 'open')
                        request = { ...request, params: { ...request.params, path: document.uri.fsPath } };
                    this.relayed.fire({ document: document.uri, direction: 'toEngine', message: request });
                    engine.post(request);
                    break;
                }
                case 'save':
                    void this.save(document.uri, message.name, message.data).then((result) =>
                        post({ type: 'saveResult', id: message.id, ...result }),
                    );
                    break;
            }
        });
    }

    private html(webview: vscode.Webview, media: vscode.Uri, document: vscode.Uri) {
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
        const name = document.path.slice(document.path.lastIndexOf('/') + 1);
        const head =
            `<meta http-equiv="Content-Security-Policy" content="${csp}">` +
            `<base href="${webview.asWebviewUri(media).toString()}/">` +
            `<meta name="tableskim-file" content="${escapeAttribute(name)}">`;
        // Webview resources are not served for CORS requests.
        return page.replace('<head>', `<head>${head}`).replace(/ crossorigin/g, '');
    }

    private async save(document: vscode.Uri, name: string, data: Uint8Array): Promise<SaveResult> {
        let target: vscode.Uri | undefined;
        try {
            target = await vscode.window.showSaveDialog({
                defaultUri: vscode.Uri.joinPath(document, '..', name),
                title: `Save ${name}`,
            });
            if (!target) return { saved: false };
            if (await isSource(document, target)) throw new Error('The file being viewed cannot be overwritten');
            await vscode.workspace.fs.writeFile(target, data instanceof Uint8Array ? data : new Uint8Array(data));
            return { saved: true };
        } catch (error) {
            return { saved: false, error: `Cannot save ${target?.fsPath ?? name}: ${String(error)}` };
        }
    }
}
