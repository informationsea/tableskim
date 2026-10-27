// @ts-check
// Talks to the TableSkim core running in a worker (tableskim-wasm.js; see
// src/wasm/wasm_worker_pre.js and src/wasm/web_session.cxx for the protocol).

/** Bits of WebSession::Event. */
export const Events = Object.freeze({
    ROWS: 1 << 0,
    ROW_COUNT: 1 << 1,
    VIEW: 1 << 2,
    SEARCH: 1 << 3,
    CHECKPOINT: 1 << 4,
    STATE: 1 << 5,
});

export class TableSkimClient
{
    /**
     * @param {string} moduleUrl the module's script, which becomes the worker
     */
    constructor(moduleUrl)
    {
        this.worker = new Worker(moduleUrl);
        this.nextId = 1;
        /** @type {Map<number, {resolve: (value: any) => void, reject: (error: Error) => void}>} */
        this.pending = new Map();
        /** @type {Set<(events: Map<number, number>) => void>} */
        this.listeners = new Set();
        /** @type {(error: string) => void} */
        this.onFatalError = () => {};
        this.ready = new Promise((resolve) =>
        {
            this.markReady = resolve;
        });
        this.worker.onmessage = (event) => this.receive(event.data);
        this.worker.onerror = (event) =>
        {
            const message = event.message || 'the worker failed';
            for (const { reject } of this.pending.values())
                reject(new Error(message));
            this.pending.clear();
            this.onFatalError(message);
        };
    }

    /**
     * @param {{type: string, json?: string}} message
     */
    receive(message)
    {
        if (message.type === 'ready')
            this.markReady?.(undefined);
        else if (message.type === 'reply' && message.json)
        {
            const reply = JSON.parse(message.json);
            const request = this.pending.get(reply.id);
            if (!request)
                return;
            this.pending.delete(reply.id);
            if (reply.error !== undefined)
                request.reject(new Error(reply.error));
            else
                request.resolve(reply.result);
        }
        else if (message.type === 'events' && message.json)
        {
            const events = new Map(Object.entries(JSON.parse(message.json)).map(([sheet, bits]) => [Number(sheet), Number(bits)]));
            for (const listener of this.listeners)
                listener(events);
        }
    }

    /**
     * Sends a request; resolves with its result, or rejects with its error.
     * @param {string} method
     * @param {object} [params]
     * @returns {Promise<any>}
     */
    call(method, params = {})
    {
        const id = this.nextId++;
        return new Promise((resolve, reject) =>
        {
            this.pending.set(id, { resolve, reject });
            this.worker.postMessage({ id, method, params });
        });
    }

    /**
     * Called with the sheets that changed (sheet id -> Events bits).
     * @param {(events: Map<number, number>) => void} listener
     * @returns {() => void} removes the listener
     */
    subscribe(listener)
    {
        this.listeners.add(listener);
        return () => this.listeners.delete(listener);
    }
}
