// Talks to the TableSkim core running in a worker (tableskim-wasm.js; see
// src/wasm/wasm_worker_pre.js for the worker's side).

import type { Method, Methods } from './protocol';

type WorkerMessage =
    | { type: 'ready' }
    | { type: 'reply'; json: string }
    | { type: 'events'; json: string };

interface Pending
{
    resolve: (value: unknown) => void;
    reject: (error: Error) => void;
}

export type EventListener = (events: Map<number, number>) => void;

export class TableSkimClient
{
    readonly ready: Promise<void>;
    onFatalError: (message: string) => void = () => {};

    private readonly worker: Worker;
    private nextId = 1;
    private readonly pending = new Map<number, Pending>();
    private readonly listeners = new Set<EventListener>();
    private markReady: () => void = () => {};

    /** @param moduleUrl the module's script, which becomes the worker */
    constructor(moduleUrl: string | URL)
    {
        // A classic worker: the module is an Emscripten script, not an ES module.
        this.worker = new Worker(moduleUrl);
        this.ready = new Promise((resolve) =>
        {
            this.markReady = resolve;
        });
        this.worker.onmessage = (event: MessageEvent<WorkerMessage>) => this.receive(event.data);
        this.worker.onerror = (event) =>
        {
            const message = event.message || 'the worker failed';
            for (const { reject } of this.pending.values())
                reject(new Error(message));
            this.pending.clear();
            this.onFatalError(message);
        };
    }

    /** Sends a request; resolves with its result, or rejects with its error. */
    call<M extends Method>(method: M, params: Methods[M]['params']): Promise<Methods[M]['result']>
    {
        const id = this.nextId++;
        return new Promise((resolve, reject) =>
        {
            this.pending.set(id, { resolve: resolve as (value: unknown) => void, reject });
            this.worker.postMessage({ id, method, params });
        });
    }

    /**
     * Called with the sheets that changed (sheet id -> Events bits).
     * Returns a function that removes the listener.
     */
    subscribe(listener: EventListener): () => void
    {
        this.listeners.add(listener);
        return () => this.listeners.delete(listener);
    }

    private receive(message: WorkerMessage)
    {
        if (message.type === 'ready')
            this.markReady();
        else if (message.type === 'reply')
        {
            const reply = JSON.parse(message.json) as { id: number; result?: unknown; error?: string };
            const request = this.pending.get(reply.id);
            if (!request)
                return;
            this.pending.delete(reply.id);
            if (reply.error !== undefined)
                request.reject(new Error(reply.error));
            else
                request.resolve(reply.result);
        }
        else if (message.type === 'events')
        {
            const bits = JSON.parse(message.json) as Record<string, number>;
            const events = new Map(Object.entries(bits).map(([sheet, value]) => [Number(sheet), value]));
            for (const listener of this.listeners)
                listener(events);
        }
    }
}
