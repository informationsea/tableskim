import { type Ref, useEffect, useRef, useState } from 'react';
import type { TableSkimClient } from '../client';
import { errorText, usePolled } from '../hooks';
import { Events, type SearchHits, type SearchState, type SheetInfo } from '../protocol';

/** A search started on a sheet; `serial` tells a new start from the last. */
export interface SearchRun {
    sheet: number;
    serial: number;
}

interface SearchPaneProps {
    client: TableSkimClient;
    info: SheetInfo;
    text: string;
    regex: boolean;
    enabled: boolean;
    run: SearchRun | null; // the search of this sheet, if any
    startError: string; // why the search could not start
    onText: (text: string) => void;
    onRegex: (regex: boolean) => void; // starts the search again, as the GUI does
    onSearch: () => void;
    onActivate: (hit: { row: number; column: number }) => void;
    inputRef?: Ref<HTMLInputElement>;
}

const ROW_HEIGHT = 24;
// Column names wider than this are cut, as in the GUI; a few are enough to measure.
const MAX_NAME_CHARS = 14;
const MEASURED_COLUMNS = 100;
// Hits asked for beyond those in view, so that scrolling a little shows values at once.
const OVERSCAN = 20;

// As GuiSearchResults::statusText().
function statusText(state: SearchState | null) {
    if (!state?.active) return '';
    if (state.error) return state.error;
    const hits = `${state.hits.toLocaleString()} ${state.hits === 1 ? 'hit' : 'hits'}`;
    if (!state.done) return `Searching... ${hits} in ${state.scannedRows.toLocaleString()} rows`;
    if (state.truncated) return `${hits} (stopped: too many hits)`;
    return hits;
}

/**
 * The Search pane, laid out and behaving as the GUI's: the text, Regex and
 * Search, the status, and the list of hits (row, column, value), whose
 * selection (a click, the arrow keys) moves the grid's cursor to the hit.
 */
export function SearchPane({
    client,
    info,
    text,
    regex,
    enabled,
    run,
    startError,
    onText,
    onRegex,
    onSearch,
    onActivate,
    inputRef,
}: SearchPaneProps) {
    const [state, setState] = useState<SearchState | null>(null);
    const [hits, setHits] = useState<SearchHits | null>(null);
    const [error, setError] = useState('');
    const [selected, setSelected] = useState<number | null>(null);
    const [viewport, setViewport] = useState({ top: 0, height: 0 });
    const list = useRef<HTMLDivElement>(null);
    const generation = useRef<number | null>(null);
    const sheet = run && run.sheet === info.sheet ? run.sheet : null;

    // The rows of the list in view, with some to spare.
    const firstShown = Math.max(0, Math.floor(viewport.top / ROW_HEIGHT) - OVERSCAN);
    const lastShown = Math.floor((viewport.top + viewport.height) / ROW_HEIGHT) + OVERSCAN;

    usePolled(
        client,
        sheet,
        Events.SEARCH | Events.ROWS | Events.VIEW,
        async () => {
            if (sheet === null) {
                setState(null);
                setHits(null);
                return;
            }
            try {
                const next = await client.call('searchState', { sheet });
                const slice = await client.call('searchHits', { sheet, first: firstShown, last: lastShown });
                // A search started over (another search, a view change) has other hits.
                if (slice.generation !== generation.current) {
                    generation.current = slice.generation;
                    setSelected(null);
                }
                setState(next);
                setHits(slice);
                setError('');
            } catch (failure) {
                setError(errorText(failure));
            }
        },
        `${sheet}:${run?.serial}:${firstShown}:${lastShown}`,
    );

    useEffect(() => {
        const element = list.current;
        if (!element) return;
        const measure = () => setViewport({ top: element.scrollTop, height: element.clientHeight });
        measure();
        const observer = new ResizeObserver(measure);
        observer.observe(element);
        return () => observer.disconnect();
    }, []);

    const count = sheet === null ? 0 : (hits?.hits ?? 0);
    const item = (index: number) => {
        const offset = index - (hits?.first ?? 0);
        return hits && offset >= 0 && offset < hits.items.length ? hits.items[offset] : null;
    };

    // Selecting a hit moves the grid's cursor to it; selecting it again
    // jumps back after the grid was scrolled away.
    const select = (index: number) => {
        const element = list.current;
        if (element) {
            const top = index * ROW_HEIGHT;
            if (top < element.scrollTop) element.scrollTop = top;
            else if (top + ROW_HEIGHT > element.scrollTop + element.clientHeight)
                element.scrollTop = top + ROW_HEIGHT - element.clientHeight;
        }
        setSelected(index);
        const hit = item(index);
        if (hit) onActivate(hit);
        else if (sheet !== null)
            // Not fetched yet: ask for it alone.
            void client
                .call('searchHits', { sheet, first: index, last: index })
                .then((slice) => {
                    const found = slice.items[0];
                    if (found && slice.generation === generation.current) onActivate(found);
                })
                .catch(() => {});
    };

    // As GuiSearchResults::fitColumns(): the row and column fit their text,
    // and the value takes the rest.
    const lastRow = hits?.items.at(-1)?.row ?? 0;
    const rowWidth = { width: `calc(${Math.max(3, String(lastRow + 1).length + 1)}ch + 0.75rem)` };
    const longestName = Math.max(6, ...info.columns.slice(0, MEASURED_COLUMNS).map((column) => column.name.length));
    const nameWidth = { width: `calc(${Math.min(MAX_NAME_CHARS, longestName)}ch + 0.75rem)` };
    const status = startError || error || statusText(sheet === null ? null : state);
    return (
        <section id="search-pane" className="flex min-h-0 flex-1 flex-col">
            <form
                id="search-form"
                className="grid grid-cols-2 gap-1.5 px-2.5 pt-2"
                autoComplete="off"
                onSubmit={(event) => {
                    event.preventDefault();
                    onSearch();
                }}
            >
                <input
                    type="search"
                    id="search-text"
                    ref={inputRef}
                    className="control col-span-2 min-w-0"
                    aria-label="Search"
                    disabled={!enabled}
                    value={text}
                    onChange={(event) => onText(event.target.value)}
                />
                <label className="flex items-center gap-1">
                    <input
                        type="checkbox"
                        id="search-regex"
                        checked={regex}
                        disabled={!enabled}
                        onChange={(event) => onRegex(event.target.checked)}
                    />
                    Regex
                </label>
                <button type="submit" id="search-button" className="control" disabled={!enabled}>
                    Search
                </button>
            </form>
            <p
                id="search-status"
                className={`truncate px-2.5 py-1 ${state?.error || startError || error ? 'text-danger' : 'text-muted'}`}
                title={status}
            >
                {status}
            </p>
            <div className="flex border-y border-line bg-header font-semibold" aria-hidden="true">
                <span className="shrink-0 px-1.5 text-right" style={rowWidth}>
                    Row
                </span>
                <span className="shrink-0 truncate px-1.5" style={nameWidth}>
                    Column
                </span>
                <span className="min-w-0 flex-1 px-1.5">Value</span>
            </div>
            <div
                id="search-results"
                ref={list}
                className="relative min-h-0 flex-1 overflow-auto outline-none focus-visible:ring-1 focus-visible:ring-accent"
                role="listbox"
                aria-label="Search hits"
                tabIndex={0}
                onScroll={(event) =>
                    setViewport({ top: event.currentTarget.scrollTop, height: event.currentTarget.clientHeight })
                }
                onKeyDown={(event) => {
                    if (count === 0) return;
                    const page = Math.max(1, Math.floor(viewport.height / ROW_HEIGHT) - 1);
                    const current = selected ?? -1;
                    const moves: Record<string, number> = {
                        ArrowDown: current + 1,
                        ArrowUp: current - 1,
                        PageDown: current + page,
                        PageUp: current - page,
                        Home: 0,
                        End: count - 1,
                    };
                    if (!(event.key in moves)) return;
                    event.preventDefault();
                    select(Math.min(count - 1, Math.max(0, moves[event.key])));
                }}
            >
                <div style={{ height: count * ROW_HEIGHT }}>
                    {Array.from({ length: Math.max(0, Math.min(count - 1, lastShown) - firstShown + 1) }, (_, i) => {
                        const index = firstShown + i;
                        const hit = item(index);
                        return (
                            <div
                                key={index}
                                className={`search-hit absolute inset-x-0 flex cursor-default border-b border-grid-line ${index === selected ? 'bg-selection' : ''}`}
                                style={{
                                    top: index * ROW_HEIGHT,
                                    height: ROW_HEIGHT,
                                    lineHeight: `${ROW_HEIGHT - 1}px`,
                                }}
                                role="option"
                                aria-selected={index === selected}
                                data-index={index}
                                onPointerDown={() => select(index)}
                            >
                                <span className="shrink-0 px-1.5 text-right tabular-nums" style={rowWidth}>
                                    {hit ? (hit.row + 1).toLocaleString() : ''}
                                </span>
                                <span className="shrink-0 truncate px-1.5" style={nameWidth}>
                                    {hit ? (info.columns[hit.column]?.name ?? '') : ''}
                                </span>
                                <span
                                    className={`min-w-0 flex-1 truncate px-1.5 ${hit?.value === null ? 'text-muted' : ''}`}
                                >
                                    {hit ? (hit.value ?? '…') : ''}
                                </span>
                            </div>
                        );
                    })}
                </div>
            </div>
        </section>
    );
}
