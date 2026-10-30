// The webview's side of the VS Code extension (vscode/): the engine runs in
// the extension host, in Node.js, and the messages go through the extension.
//
// To the extension:   {type: 'engine', message}   a request for the engine,
//                     {type: 'save', name, data}  a file to save (Uint8Array),
//                     {type: 'pick'}              let the user open another file.
// From the extension: {type: 'engine', message}   what the engine posted,
//                     {type: 'engineError', message}.

import type { EnginePort, WorkerMessage } from '../client';

interface VsCodeApi {
    postMessage(message: unknown): void;
}

declare function acquireVsCodeApi(): VsCodeApi;

type FromExtension = { type: 'engine'; message: WorkerMessage } | { type: 'engineError'; message: string };

// acquireVsCodeApi() may be called once only.
const vscode = acquireVsCodeApi();

export function postToExtension(message: unknown) {
    vscode.postMessage(message);
}

export class ExtensionPort implements EnginePort {
    onmessage: ((event: MessageEvent<WorkerMessage>) => void) | null = null;
    onerror: ((event: ErrorEvent) => void) | null = null;

    constructor() {
        window.addEventListener('message', (event: MessageEvent<FromExtension>) => {
            const data = event.data;
            if (data.type === 'engine') this.onmessage?.(new MessageEvent('message', { data: data.message }));
            else if (data.type === 'engineError') this.onerror?.(new ErrorEvent('error', { message: data.message }));
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
