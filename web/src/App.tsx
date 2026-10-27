// The TableSkim web app: one file at a time, read in this browser by the
// core running in a worker.

import { useCallback, useEffect, useRef, useState } from 'react';
import type { TableSkimClient } from './client';
import { CheckpointBanner, MessageBanner } from './components/Banners';
import { DropOverlay, EmptyState } from './components/EmptyState';
import { FilterBar } from './components/FilterBar';
import { SheetTabs } from './components/SheetTabs';
import { StatusBar } from './components/StatusBar';
import { Toolbar } from './components/Toolbar';
import type { Cell, Range } from './grid/controller';
import { Grid, type GridHandle } from './grid/Grid';
import { type BookInfo, Events, type Filter, type FilterOpInfo, type HeaderMode, type SheetInfo } from './protocol';

// Copying more rows than this would build a huge string in the worker.
const MAX_COPY_ROWS = 100000;

type Phase =
    | { kind: 'starting' }
    | { kind: 'unsupported' }
    | { kind: 'idle' }
    | { kind: 'opening'; name: string }
    | { kind: 'failed'; name: string }
    | { kind: 'open' };

function errorText(error: unknown)
{
    return error instanceof Error ? error.message : String(error);
}

function emptyTitle(phase: Phase, info: SheetInfo | null)
{
    switch (phase.kind)
    {
    case 'starting': return 'Loading…';
    case 'unsupported': return 'This page must be served cross-origin isolated';
    case 'idle': return 'Open a file, or drop one here';
    case 'opening': return `Opening ${phase.name}…`;
    case 'failed': return `Cannot open ${phase.name}`;
    case 'open':
        if (!info || info.state === 'opening')
            return `Opening ${info?.name ?? ''}…`;
        if (info.state === 'error')
            return `Cannot read ${info.name}`;
        return `${info.name} is empty`;
    }
}

export function App({ client }: { client: TableSkimClient })
{
    const [phase, setPhase] = useState<Phase>({ kind: 'starting' });
    const [filterOps, setFilterOps] = useState<FilterOpInfo[]>([]);
    const [file, setFile] = useState<File | null>(null);
    const [book, setBook] = useState<BookInfo | null>(null);
    const [headerMode, setHeaderMode] = useState<HeaderMode>('auto');
    const [info, setInfo] = useState<SheetInfo | null>(null);
    const [sheetKey, setSheetKey] = useState(0);
    const [dataVersion, setDataVersion] = useState(0);
    const [viewVersion, setViewVersion] = useState(0);
    const [message, setMessage] = useState('');
    const [filterOpen, setFilterOpen] = useState(false);
    const [cellStatus, setCellStatus] = useState('');
    const [search, setSearch] = useState({ text: '', caseSensitive: false, regex: false });
    const [searchStatus, setSearchStatus] = useState('');
    const [dragging, setDragging] = useState(false);

    const grid = useRef<GridHandle>(null);
    const searchInput = useRef<HTMLInputElement>(null);
    // Mutable state of the flows below, which outlive a render.
    const flow = useRef({
        epoch: 0, // bumped by every open and sheet change, so that late replies are dropped
        info: null as SheetInfo | null,
        infoLoading: false,
        infoDirty: false,
        searchActive: false,
        pendingFind: null as { forward: boolean } | null,
    });

    const showInfo = useCallback((next: SheetInfo) =>
    {
        flow.current.info = next;
        setInfo(next);
        if (next.state === 'error')
            setMessage(next.error ?? 'The sheet cannot be read.');
        if (next.viewError)
            setMessage(`The sort or filter failed: ${next.viewError}`);
        if (next.view.filter?.conditions.length && next.capabilities.filter)
            setFilterOpen(true);
    }, []);

    // Asks for the sheet's state again, one request at a time.
    const refreshInfo = useCallback(async () =>
    {
        const state = flow.current;
        if (state.infoLoading)
        {
            state.infoDirty = true;
            return;
        }
        const current = state.info;
        if (!current)
            return;
        const epoch = state.epoch;
        state.infoLoading = true;
        try
        {
            do
            {
                state.infoDirty = false;
                const next = await client.call('info', { sheet: current.sheet });
                if (epoch !== state.epoch)
                    return;
                showInfo(next);
            } while (state.infoDirty);
        }
        catch (error)
        {
            setMessage(errorText(error));
        }
        finally
        {
            state.infoLoading = false;
        }
    }, [client, showInfo]);

    const resetSearch = useCallback(() =>
    {
        flow.current.searchActive = false;
        flow.current.pendingFind = null;
        setSearchStatus('');
    }, []);

    const selectSheet = useCallback(async (name: string) =>
    {
        const epoch = ++flow.current.epoch;
        resetSearch();
        try
        {
            const next = await client.call('sheet', { name });
            if (epoch !== flow.current.epoch)
                return;
            showInfo(next);
            setSheetKey((key) => key + 1);
            grid.current?.focus();
        }
        catch (error)
        {
            setMessage(errorText(error));
        }
    }, [client, resetSearch, showInfo]);

    const openFile = useCallback(async (opened: File, header: HeaderMode = 'auto') =>
    {
        const epoch = ++flow.current.epoch;
        flow.current.info = null;
        resetSearch();
        setFile(opened);
        setBook(null);
        setInfo(null);
        setHeaderMode(header);
        setMessage('');
        setCellStatus('');
        setPhase({ kind: 'opening', name: opened.name });
        document.title = `${opened.name} – TableSkim`;
        try
        {
            const next = await client.call('open', { file: opened, header });
            if (epoch !== flow.current.epoch)
                return;
            setBook(next);
            if (next.sheets.length === 0)
                throw new Error('The file has no sheets.');
            setPhase({ kind: 'open' });
            await selectSheet(next.sheets[0]);
        }
        catch (error)
        {
            if (epoch !== flow.current.epoch)
                return;
            setPhase({ kind: 'failed', name: opened.name });
            setMessage(errorText(error));
        }
    }, [client, resetSearch, selectSheet]);

    const showSearchState = useCallback(async () =>
    {
        const current = flow.current.info;
        if (!current || !flow.current.searchActive)
            return;
        const state = await client.call('searchState', { sheet: current.sheet });
        if (state.error)
            setSearchStatus(state.error);
        else if (!state.done)
            setSearchStatus(`${state.hits.toLocaleString()} hits so far…`);
        else if (state.hits === 0)
            setSearchStatus('No hits');
        else
            setSearchStatus(`${state.hits.toLocaleString()}${state.truncated ? '+' : ''} hits`);
        const pending = flow.current.pendingFind;
        if (pending && (state.hits > 0 || state.done))
        {
            flow.current.pendingFind = null;
            if (state.hits > 0)
                await findHit(pending.forward, true);
        }
    // findHit and showSearchState call each other; both only read refs, so
    // the findHit of an earlier render does as well as the latest.
    }, [client]);

    // Moves to the next (or previous) hit from the cursor; includeFrom
    // counts a hit at the cursor itself.
    const findHit = async (forward: boolean, includeFrom = false) =>
    {
        const current = flow.current.info;
        const cursor = grid.current?.cursor();
        if (!current || !cursor || !flow.current.searchActive)
            return;
        const hit = await client.call('findHit', { sheet: current.sheet, row: cursor.row, column: cursor.column, forward, includeFrom });
        if (hit.index !== null)
        {
            flow.current.pendingFind = null;
            grid.current?.select({ row: hit.row, column: hit.column });
        }
        else if (hit.pending)
            flow.current.pendingFind = { forward }; // tried again as the search goes on
        await showSearchState();
    };

    const startSearch = async () =>
    {
        const current = flow.current.info;
        if (!current || !search.text)
            return;
        try
        {
            await client.call('search', {
                sheet: current.sheet,
                text: search.text,
                caseSensitive: search.caseSensitive,
                regex: search.regex,
            });
            flow.current.searchActive = true;
            await findHit(true, true);
        }
        catch (error)
        {
            setSearchStatus(errorText(error));
        }
    };

    const changeSearch = (change: Partial<typeof search>) =>
    {
        // A changed query or option is a new search.
        const current = flow.current.info;
        if (flow.current.searchActive && current)
            client.call('cancelSearch', { sheet: current.sheet }).catch(() => {});
        resetSearch();
        setSearch((previous) => ({ ...previous, ...change }));
    };

    const sortBy = async (column: number) =>
    {
        const current = flow.current.info;
        if (!current?.capabilities.sort)
            return;
        try
        {
            const reply = await client.call('sortCycle', { sheet: current.sheet, column });
            if (flow.current.info?.sheet === current.sheet)
            {
                showInfo({ ...flow.current.info, view: reply.view });
                refreshInfo();
            }
        }
        catch (error)
        {
            setMessage(errorText(error));
        }
    };

    // Applies a filter with the current sort.
    const setFilter = async (filter: Filter | null) =>
    {
        const current = flow.current.info;
        if (!current)
            return;
        try
        {
            await client.call('setView', { sheet: current.sheet, sort: current.view.sort, filter });
            setMessage('');
            refreshInfo();
        }
        catch (error)
        {
            setMessage(errorText(error));
        }
    };

    const showCell = (cell: Cell, range: Range) =>
    {
        const name = flow.current.info?.columns[cell.column]?.name ?? '';
        let text = `Row ${(cell.row + 1).toLocaleString()}, ${name}`;
        if (range.bottom > range.top || range.right > range.left)
            text += ` (${(range.bottom - range.top + 1).toLocaleString()} × ${range.right - range.left + 1} selected)`;
        setCellStatus(text);
    };

    const copy = async (range: Range) =>
    {
        const current = flow.current.info;
        if (!current)
            return;
        if (range.bottom - range.top + 1 > MAX_COPY_ROWS)
        {
            setMessage(`Select at most ${MAX_COPY_ROWS.toLocaleString()} rows to copy.`);
            return;
        }
        try
        {
            const copied = await client.call('copy', { sheet: current.sheet, ...range });
            await navigator.clipboard.writeText(copied.text);
            setCellStatus(copied.skippedRows > 0
                ? `Copied ${copied.copiedRows} rows; ${copied.skippedRows} not loaded yet were left out`
                : `Copied ${copied.copiedRows.toLocaleString()} rows`);
        }
        catch (error)
        {
            setMessage(`Cannot copy: ${errorText(error)}`);
        }
    };

    const fetchRows = useCallback((first: number, last: number, firstColumn: number, lastColumn: number) =>
    {
        const current = flow.current.info;
        if (!current)
            return Promise.reject(new Error('no sheet'));
        const epoch = flow.current.epoch;
        return client.call('rows', { sheet: current.sheet, first, last, firstColumn, lastColumn }).then((rows) =>
        {
            if (epoch !== flow.current.epoch)
                throw new Error('stale');
            return rows;
        });
    }, [client]);

    // Start: the engine, then the filter operators it offers.
    useEffect(() =>
    {
        if (!self.crossOriginIsolated)
        {
            setPhase({ kind: 'unsupported' });
            setMessage('The engine needs SharedArrayBuffer: serve the page with the headers ' +
                'Cross-Origin-Opener-Policy: same-origin and Cross-Origin-Embedder-Policy: require-corp.');
            return;
        }
        client.onFatalError = (text) => setMessage(`The engine stopped: ${text}. Reload the page to start again.`);
        let cancelled = false;
        client.ready.then(() => client.call('filterOps', {})).then((ops) =>
        {
            if (cancelled)
                return;
            setFilterOps(ops);
            setPhase((current) => (current.kind === 'starting' ? { kind: 'idle' } : current));
            document.body.dataset.ready = 'true';
        }, (error: unknown) => setMessage(errorText(error)));
        return () =>
        {
            cancelled = true;
        };
    }, [client]);

    // The model's events: look again at what changed.
    useEffect(() => client.subscribe((events) =>
    {
        const current = flow.current.info;
        const bits = current && events.get(current.sheet);
        if (!bits)
            return;
        if (bits & Events.VIEW)
            setViewVersion((version) => version + 1);
        else if (bits & Events.ROWS)
            setDataVersion((version) => version + 1);
        if (bits & (Events.STATE | Events.ROW_COUNT | Events.VIEW | Events.CHECKPOINT))
            refreshInfo();
        if (bits & (Events.SEARCH | Events.VIEW))
            showSearchState();
    }), [client, refreshInfo, showSearchState]);

    // Ctrl+F and Ctrl+O, and dropping a file anywhere.
    useEffect(() =>
    {
        let depth = 0;
        const keyDown = (event: KeyboardEvent) =>
        {
            const ctrl = event.ctrlKey || event.metaKey;
            if (ctrl && event.key.toLowerCase() === 'f' && searchInput.current && !searchInput.current.disabled)
            {
                event.preventDefault();
                searchInput.current.focus();
                searchInput.current.select();
            }
            else if (ctrl && event.key.toLowerCase() === 'o')
            {
                event.preventDefault();
                document.getElementById('file-input')?.click();
            }
        };
        const dragEnter = (event: DragEvent) =>
        {
            if (event.dataTransfer?.types.includes('Files'))
            {
                depth++;
                setDragging(true);
            }
        };
        const dragLeave = () =>
        {
            depth = Math.max(0, depth - 1);
            if (depth === 0)
                setDragging(false);
        };
        const dragOver = (event: DragEvent) => event.preventDefault();
        const drop = (event: DragEvent) =>
        {
            event.preventDefault();
            depth = 0;
            setDragging(false);
            const dropped = event.dataTransfer?.files[0];
            if (dropped)
                openFile(dropped);
        };
        document.addEventListener('keydown', keyDown);
        window.addEventListener('dragenter', dragEnter);
        window.addEventListener('dragleave', dragLeave);
        window.addEventListener('dragover', dragOver);
        window.addEventListener('drop', drop);
        return () =>
        {
            document.removeEventListener('keydown', keyDown);
            window.removeEventListener('dragenter', dragEnter);
            window.removeEventListener('dragleave', dragLeave);
            window.removeEventListener('dragover', dragOver);
            window.removeEventListener('drop', drop);
        };
    }, [openFile]);

    const fileText = !file ? '' : book?.textEncoding ? `${file.name} (${book.textEncoding})` : file.name;
    const rowsText = info
        ? info.rowsText + (info.pending ? ' (applying…)' : '') + (info.readingStopped ? ' (reading stopped)' : '')
        : '';
    const showGrid = phase.kind === 'open' && info !== null && info.columns.length > 0;

    return (
        <div className="flex h-full flex-col">
            <Toolbar
                onOpen={(opened) => openFile(opened)}
                showHeaderMode={!!book?.usesHeaderOption}
                headerMode={headerMode}
                onHeaderMode={(mode) => file && openFile(file, mode)}
                filterEnabled={!!info?.capabilities.filter}
                filterOpen={filterOpen}
                onToggleFilter={() => setFilterOpen((open) => !open)}
                search={{
                    ...search,
                    enabled: !!info?.capabilities.search,
                    status: searchStatus,
                    onChange: changeSearch,
                    onFind: (forward) => (flow.current.searchActive ? findHit(forward) : startSearch()),
                    onLeave: () => grid.current?.focus(),
                    inputRef: searchInput,
                }}
            />
            {filterOpen && info && (
                <FilterBar columns={info.columns} filter={info.view.filter} ops={filterOps} onChange={setFilter} />
            )}
            <CheckpointBanner
                checkpoint={info?.checkpoint ?? null}
                onContinue={() => info && client.call('continueReading', { sheet: info.sheet }).then(refreshInfo)}
                onStop={() => info && client.call('stopReading', { sheet: info.sheet }).then(refreshInfo)}
            />
            <MessageBanner message={message} />
            <SheetTabs sheets={book?.sheets ?? []} current={info?.name ?? null} onSelect={selectSheet} />
            <main className="relative min-h-0 flex-1">
                <Grid
                    ref={grid}
                    info={phase.kind === 'open' ? info : null}
                    sheetKey={sheetKey}
                    dataVersion={dataVersion}
                    viewVersion={viewVersion}
                    fetchRows={fetchRows}
                    onSort={sortBy}
                    onSelect={showCell}
                    onCopy={copy}
                />
                {!showGrid && <EmptyState title={emptyTitle(phase, info)} />}
                {dragging && <DropOverlay />}
            </main>
            <StatusBar
                file={fileText}
                rows={rowsText}
                filter={info?.filterText ? `Filter: ${info.filterText}` : ''}
                cell={cellStatus}
            />
        </div>
    );
}
