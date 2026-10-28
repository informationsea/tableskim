import { useEffect, useRef } from 'react';
import type { TableSkimClient } from './client';
import type { SheetInfo } from './protocol';

/**
 * Calls `handler` when the sheet (or SESSION) reports any of the event
 * bits in `mask`; the latest handler is called.
 */
export function useEvents(client: TableSkimClient, sheet: number | null, mask: number, handler: () => void) {
    const latest = useRef(handler);
    useEffect(() => {
        latest.current = handler;
    });
    useEffect(() => {
        if (sheet === null) return;
        return client.subscribe((events) => {
            if ((events.get(sheet) ?? 0) & mask) latest.current();
        });
    }, [client, sheet, mask]);
}

/**
 * Runs `poll` now and on each of those events, one call at a time: an
 * event during a call runs it once more afterwards. `key` changes start
 * it over.
 */
export function usePolled(
    client: TableSkimClient,
    sheet: number | null,
    mask: number,
    poll: () => Promise<void>,
    key: unknown,
) {
    const latest = useRef(poll);
    const state = useRef({ running: false, again: false });
    useEffect(() => {
        latest.current = poll;
    });
    const run = useRef(async () => {
        const flags = state.current;
        if (flags.running) {
            flags.again = true;
            return;
        }
        flags.running = true;
        try {
            do {
                flags.again = false;
                await latest.current();
            } while (flags.again);
        } finally {
            flags.running = false;
        }
    });
    useEffect(() => {
        void run.current();
    }, [key]);
    useEvents(client, sheet, mask, () => void run.current());
}

export function errorText(error: unknown) {
    return error instanceof Error ? error.message : String(error);
}

/** A, B, ..., Z, AA, ...: a column's letter. */
export function columnLetter(column: number): string {
    let letters = '';
    for (let n = column + 1; n > 0; n = Math.floor((n - 1) / 26))
        letters = String.fromCharCode(65 + ((n - 1) % 26)) + letters;
    return letters;
}

/** A column's label: header names may repeat, so the letter follows. */
export function columnLabel(info: SheetInfo, column: number): string {
    const name = info.columns[column]?.name ?? '';
    return info.hasHeader ? `${name} (${columnLetter(column)})` : name;
}
