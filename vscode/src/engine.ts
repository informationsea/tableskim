// The TableSkim engine: tableskim-wasm-node.js (src/wasm/) run in a
// worker_threads Worker, which Emscripten's own threads are started from.
// It answers the web app's protocol (web/src/protocol.ts); this file has
// no dependency on VS Code, so that the tests can run it in Node.js.

import { Worker } from 'node:worker_threads';

/** What the engine posts: {type: 'ready' | 'reply' | 'events' | 'file', ...}. */
export type EngineMessage = { type: string; [key: string]: unknown };

export class Engine {
    private readonly worker: Worker;
    private stopped = false;

    /**
     * @param modulePath tableskim-wasm-node.js
     * @param onMessage called with each message the engine posts
     * @param onError called once if the engine stops, after which it answers nothing more
     */
    constructor(modulePath: string, onMessage: (message: EngineMessage) => void, onError: (text: string) => void) {
        this.worker = new Worker(modulePath);
        this.worker.on('message', onMessage);
        const fail = (text: string) => {
            if (this.stopped) return;
            this.stopped = true;
            onError(text);
        };
        this.worker.on('error', (error) => fail(error.message));
        this.worker.on('exit', (code) => fail(`the engine exited (${code})`));
    }

    /**
     * Sends a request {id, method, params}. An "open" names the file on
     * this machine by params.path; the engine mounts its directory.
     */
    post(message: unknown) {
        this.worker.postMessage(message);
    }

    dispose() {
        this.stopped = true;
        void this.worker.terminate();
    }
}
