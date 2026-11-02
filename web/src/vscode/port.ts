// The webview's side of the VS Code extension (vscode/): the engine runs in
// the extension host, in Node.js, and the messages go through the extension.
//
// To the extension:   {type: 'hello'}             the page listens: start an engine,
//                     {type: 'engine', message}   a request for the engine,
//                     {type: 'save', id, name, data} a file to save (Uint8Array).
// From the extension: {type: 'engine', message}   what the engine posted,
//                     {type: 'engineError', message},
//                     {type: 'saveResult', id, saved, error?} the save's outcome.

import type { EnginePort, WorkerMessage } from '../client';

interface VsCodeApi {
    postMessage(message: unknown): void;
}

declare function acquireVsCodeApi(): VsCodeApi;

type FromExtension =
    | { type: 'engine'; message: WorkerMessage }
    | { type: 'engineError'; message: string }
    | { type: 'saveResult'; id: number; saved: boolean; error?: string };

// acquireVsCodeApi() may be called once only.
const vscode = acquireVsCodeApi();

export function postToExtension(message: unknown) {
    vscode.postMessage(message);
}

export class ExtensionPort implements EnginePort {
    onmessage: ((event: MessageEvent<WorkerMessage>) => void) | null = null;
    onerror: ((event: ErrorEvent) => void) | null = null;
    private nextSaveId = 1;
    private readonly saves = new Map<number, { resolve: (saved: boolean) => void; reject: (error: Error) => void }>();

    constructor() {
        window.addEventListener('message', (event: MessageEvent<FromExtension>) => {
            const data = event.data;
            if (data.type === 'engine') this.onmessage?.(new MessageEvent('message', { data: data.message }));
            else if (data.type === 'engineError') this.onerror?.(new ErrorEvent('error', { message: data.message }));
            else if (data.type === 'saveResult') {
                const pending = this.saves.get(data.id);
                this.saves.delete(data.id);
                if (data.error !== undefined) pending?.reject(new Error(data.error));
                else pending?.resolve(data.saved);
            }
        });
        // Every load of the page gets an engine of its own (the extension
        // starts it on this), so that a reloaded page starts from "ready".
        vscode.postMessage({ type: 'hello' });
    }

    async saveFile(blob: Blob, name: string): Promise<boolean> {
        const data = new Uint8Array(await blob.arrayBuffer());
        const id = this.nextSaveId++;
        return new Promise((resolve, reject) => {
            this.saves.set(id, { resolve, reject });
            vscode.postMessage({ type: 'save', id, name, data });
        });
    }

    postMessage(message: unknown) {
        const request = message as { id: number; method: string; params: Record<string, unknown> };
        if (request.method === 'open') {
            // The extension opens its document by its path: a File cannot cross.
            const params = { ...request.params };
            delete params.file;
            vscode.postMessage({ type: 'engine', message: { ...request, params } });
        } else vscode.postMessage({ type: 'engine', message: request });
    }
}
