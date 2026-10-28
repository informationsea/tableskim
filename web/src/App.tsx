// The TableSkim web app: one file at a time, read in this browser by the
// core running in a worker.

import { useCallback, useEffect, useRef, useState } from 'react';
import type { TableSkimClient } from './client';
import { CheckpointBanner, MessageBanner } from './components/Banners';
import { DropOverlay, EmptyState } from './components/EmptyState';
import { ExportDialog } from './components/ExportDialog';
import { FilterBar } from './components/FilterBar';
import { FormatDialog } from './components/FormatDialog';
import { GoToDialog } from './components/GoToDialog';
import { GroupByDialog } from './components/GroupByDialog';
import { HistogramDialog } from './components/HistogramDialog';
import { Menu, type MenuItem } from './components/Menu';
import { ScatterDialog } from './components/ScatterDialog';
import { SheetTabs } from './components/SheetTabs';
import { SqlPanel } from './components/SqlPanel';
import { StatsPanel } from './components/StatsPanel';
import { StatusBar } from './components/StatusBar';
import { type Tool, Toolbar } from './components/Toolbar';
import type { Cell, ContextTarget, Range } from './grid/controller';
import { Grid, type GridHandle } from './grid/Grid';
import { errorText } from './hooks';
import {
    type BookInfo,
    type Condition,
    Events,
    type Filter,
    type FilterOpInfo,
    type HeaderMode,
    type SheetInfo,
    type SortKey,
} from './protocol';

// Copying more rows than this would build a huge string in the worker.
const MAX_COPY_ROWS = 100000;

type Phase =
    | { kind: 'starting' }
    | { kind: 'unsupported' }
    | { kind: 'idle' }
    | { kind: 'opening'; name: string }
    | { kind: 'failed'; name: string }
    | { kind: 'open' };

type DialogKind = 'histogram' | 'scatter' | 'export' | 'groupBy' | 'format' | 'goTo';

// Hidden and frozen columns: part of what a sheet shows, kept here.
interface Layout {
    hidden: Set<number>;
    frozen: number;
}

const NO_LAYOUT: Layout = { hidden: new Set(), frozen: 0 };

function emptyTitle(phase: Phase, info: SheetInfo | null) {
    switch (phase.kind) {
        case 'starting':
            return 'Loading…';
        case 'unsupported':
            return 'This page must be served cross-origin isolated';
        case 'idle':
            return 'Open a file, or drop one here';
        case 'opening':
            return `Opening ${phase.name}…`;
        case 'failed':
            return `Cannot open ${phase.name}`;
        case 'open':
            if (!info || info.state === 'opening') return `Opening ${info?.name ?? ''}…`;
            if (info.state === 'error') return `Cannot read ${info.name}`;
            return `${info.name} is empty`;
    }
}

// Threads need SharedArrayBuffer, which only a cross-origin isolated page has.
const ISOLATED = self.crossOriginIsolated;
const ISOLATION_MESSAGE =
    'The engine needs SharedArrayBuffer: serve the page with the headers ' +
    'Cross-Origin-Opener-Policy: same-origin and Cross-Origin-Embedder-Policy: require-corp.';

export function App({ client }: { client: TableSkimClient }) {
    const [phase, setPhase] = useState<Phase>({ kind: ISOLATED ? 'starting' : 'unsupported' });
    const [filterOps, setFilterOps] = useState<FilterOpInfo[]>([]);
    const [file, setFile] = useState<File | null>(null);
    const [book, setBook] = useState<BookInfo | null>(null);
    const [headerMode, setHeaderMode] = useState<HeaderMode>('auto');
    const [info, setInfo] = useState<SheetInfo | null>(null);
    const [sheetKey, setSheetKey] = useState(0);
    const [dataVersion, setDataVersion] = useState(0);
    const [viewVersion, setViewVersion] = useState(0);
    const [message, setMessage] = useState(ISOLATED ? '' : ISOLATION_MESSAGE);
    const [filterOpen, setFilterOpen] = useState(false);
    const [cellStatus, setCellStatus] = useState('');
    const [search, setSearch] = useState({ text: '', caseSensitive: false, regex: false });
    const [searchStatus, setSearchStatus] = useState('');
    const [dragging, setDragging] = useState(false);
    const [layout, setLayout] = useState<Layout>(NO_LAYOUT);
    const [cursorColumn, setCursorColumn] = useState(0);
    const [statsOpen, setStatsOpen] = useState(false);
    const [sqlOpen, setSqlOpen] = useState(false);
    const [dialog, setDialog] = useState<{ kind: DialogKind; column: number } | null>(null);
    const [menu, setMenu] = useState<{ x: number; y: number; items: MenuItem[] } | null>(null);

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
        lastSql: '', // run again after the file is opened again
    });

    const showInfo = useCallback((next: SheetInfo) => {
        flow.current.info = next;
        setInfo(next);
        if (next.state === 'error') setMessage(next.error ?? 'The sheet cannot be read.');
        if (next.viewError) setMessage(`The sort or filter failed: ${next.viewError}`);
        if (next.view.filter?.conditions.length && next.capabilities.filter) setFilterOpen(true);
    }, []);

    // Asks for the sheet's state again, one request at a time.
    const refreshInfo = useCallback(async () => {
        const state = flow.current;
        if (state.infoLoading) {
            state.infoDirty = true;
            return;
        }
        state.infoLoading = true;
        try {
            do {
                state.infoDirty = false;
                // The sheet shown now: another may have been chosen while a
                // request was out, whose reply is then dropped, not the ask.
                const current = state.info;
                if (!current) return;
                const epoch = state.epoch;
                try {
                    const next = await client.call('info', { sheet: current.sheet });
                    if (epoch === state.epoch) showInfo(next);
                    else state.infoDirty = true;
                } catch (error) {
                    if (epoch === state.epoch) setMessage(errorText(error));
                    else state.infoDirty = true;
                }
            } while (state.infoDirty);
        } finally {
            state.infoLoading = false;
        }
    }, [client, showInfo]);

    const resetSearch = useCallback(() => {
        flow.current.searchActive = false;
        flow.current.pendingFind = null;
        setSearchStatus('');
    }, []);

    const selectSheet = useCallback(
        async (name: string) => {
            const epoch = ++flow.current.epoch;
            resetSearch();
            try {
                const next = await client.call('sheet', { name });
                if (epoch !== flow.current.epoch) return;
                showInfo(next);
                setSheetKey((key) => key + 1);
                setLayout(NO_LAYOUT);
                setCursorColumn(0);
                setDialog(null);
                grid.current?.focus();
                // The event that it opened may have come before this reply,
                // when another sheet was shown, and been dropped: ask again.
                if (next.state === 'opening') void refreshInfo();
            } catch (error) {
                setMessage(errorText(error));
            }
        },
        [client, refreshInfo, resetSearch, showInfo],
    );

    const openFile = useCallback(
        // again: the same file with another option, whose SQL result is made again
        async (opened: File, header: HeaderMode = 'auto', again = false) => {
            const epoch = ++flow.current.epoch;
            flow.current.info = null;
            resetSearch();
            setFile(opened);
            setBook(null);
            setInfo(null);
            setHeaderMode(header);
            setMessage('');
            setCellStatus('');
            setDialog(null);
            if (!again) {
                flow.current.lastSql = '';
                setSqlOpen(false);
            }
            setPhase({ kind: 'opening', name: opened.name });
            document.title = `${opened.name} – TableSkim`;
            try {
                const next = await client.call('open', { file: opened, header });
                if (epoch !== flow.current.epoch) return;
                if (again && flow.current.lastSql && next.supportsSql) {
                    // The result's view died with the book.
                    try {
                        next.sheets = (await client.call('sql', { sql: flow.current.lastSql })).sheets;
                    } catch {
                        flow.current.lastSql = '';
                    }
                    if (epoch !== flow.current.epoch) return;
                }
                setBook(next);
                if (next.sheets.length === 0) throw new Error('The file has no sheets.');
                setPhase({ kind: 'open' });
                await selectSheet(next.sheets[0]);
            } catch (error) {
                if (epoch !== flow.current.epoch) return;
                setPhase({ kind: 'failed', name: opened.name });
                setMessage(errorText(error));
            }
        },
        [client, resetSearch, selectSheet],
    );

    // Shows how far the search is; returns its state.
    const updateSearchStatus = useCallback(async () => {
        const current = flow.current.info;
        if (!current || !flow.current.searchActive) return null;
        const state = await client.call('searchState', { sheet: current.sheet });
        if (state.error) setSearchStatus(state.error);
        else if (!state.done) setSearchStatus(`${state.hits.toLocaleString()} hits so far…`);
        else if (state.hits === 0) setSearchStatus('No hits');
        else setSearchStatus(`${state.hits.toLocaleString()}${state.truncated ? '+' : ''} hits`);
        return state;
    }, [client]);

    // Moves to the next (or previous) hit from the cursor; includeFrom
    // counts a hit at the cursor itself.
    const findHit = useCallback(
        async (forward: boolean, includeFrom = false) => {
            const current = flow.current.info;
            const cursor = grid.current?.cursor();
            if (!current || !cursor || !flow.current.searchActive) return;
            const hit = await client.call('findHit', {
                sheet: current.sheet,
                row: cursor.row,
                column: cursor.column,
                forward,
                includeFrom,
            });
            if (hit.index !== null) {
                flow.current.pendingFind = null;
                grid.current?.select({ row: hit.row, column: hit.column });
            } else if (hit.pending) flow.current.pendingFind = { forward }; // tried again as the search goes on
            await updateSearchStatus();
        },
        [client, updateSearchStatus],
    );

    // The search went on: show it, and move to the hit that was asked for
    // before the search had found one.
    const searchProgressed = useCallback(async () => {
        try {
            const state = await updateSearchStatus();
            const pending = flow.current.pendingFind;
            if (state && pending && (state.hits > 0 || state.done)) {
                flow.current.pendingFind = null;
                if (state.hits > 0) await findHit(pending.forward, true);
            }
        } catch (error) {
            setSearchStatus(errorText(error));
        }
    }, [findHit, updateSearchStatus]);

    const startSearch = async () => {
        const current = flow.current.info;
        if (!current || !search.text) return;
        try {
            await client.call('search', {
                sheet: current.sheet,
                text: search.text,
                caseSensitive: search.caseSensitive,
                regex: search.regex,
            });
            flow.current.searchActive = true;
            await findHit(true, true);
        } catch (error) {
            setSearchStatus(errorText(error));
        }
    };

    const changeSearch = (change: Partial<typeof search>) => {
        // A changed query or option is a new search.
        const current = flow.current.info;
        if (flow.current.searchActive && current) client.call('cancelSearch', { sheet: current.sheet }).catch(() => {});
        resetSearch();
        setSearch((previous) => ({ ...previous, ...change }));
    };

    const sortBy = async (column: number) => {
        const current = flow.current.info;
        if (!current?.capabilities.sort) return;
        try {
            const reply = await client.call('sortCycle', { sheet: current.sheet, column });
            if (flow.current.info?.sheet === current.sheet) {
                showInfo({ ...flow.current.info, view: reply.view });
                void refreshInfo();
            }
        } catch (error) {
            setMessage(errorText(error));
        }
    };

    // Applies a filter with the current sort.
    const setFilter = async (filter: Filter | null) => {
        const current = flow.current.info;
        if (!current) return;
        try {
            await client.call('setView', { sheet: current.sheet, sort: current.view.sort, filter });
            setMessage('');
            void refreshInfo();
        } catch (error) {
            setMessage(errorText(error));
        }
    };

    // Adds conditions to the view's filter (a histogram's bin, a cell's value...).
    const addConditions = async (conditions: Condition[]) => {
        const current = flow.current.info;
        if (!current?.capabilities.filter || conditions.length === 0) return;
        const filter = current.view.filter ?? { combine: 'and' as const, conditions: [] };
        const merged = [...filter.conditions];
        for (const condition of conditions) {
            if (
                !merged.some(
                    (c) => c.column === condition.column && c.op === condition.op && c.value === condition.value,
                )
            )
                merged.push(condition);
        }
        setFilterOpen(true);
        await setFilter({ ...filter, conditions: merged });
    };

    const setSort = async (sort: SortKey[]) => {
        const current = flow.current.info;
        if (!current?.capabilities.sort) return;
        try {
            await client.call('setView', { sheet: current.sheet, sort, filter: current.view.filter });
            void refreshInfo();
        } catch (error) {
            setMessage(errorText(error));
        }
    };

    const cellFilter = async (cell: Cell, equal: boolean) => {
        const current = flow.current.info;
        if (!current) return;
        try {
            const reply = await client.call('cellFilter', { sheet: current.sheet, ...cell, equal });
            await addConditions(reply.conditions);
        } catch (error) {
            setMessage(errorText(error));
        }
    };

    const hideColumn = (column: number) => {
        const current = flow.current.info;
        if (!current) return;
        const hidden = new Set(layout.hidden).add(column);
        if (hidden.size >= current.columns.length) {
            setMessage('At least one column stays shown.');
            return;
        }
        setLayout({ ...layout, hidden });
    };

    const showResult = async (sheet: string, sheets: string[]) => {
        setBook((previous) => previous && { ...previous, sheets });
        if (sheet) await selectSheet(sheet);
    };

    const columnMenu = (column: number): MenuItem[] => {
        const current = flow.current.info;
        const sorted = current?.view.sort.some((key) => key.column === column);
        const items: MenuItem[] = [];
        if (current?.capabilities.sort) {
            items.push(
                { label: 'Sort Ascending', onSelect: () => void setSort([{ column, ascending: true }]) },
                { label: 'Sort Descending', onSelect: () => void setSort([{ column, ascending: false }]) },
            );
            if (sorted) items.push({ label: 'Clear Sort', onSelect: () => void setSort([]) });
            items.push('separator');
        }
        items.push(
            { label: 'Statistics', onSelect: () => setStatsOpen(true) },
            { label: 'Histogram…', onSelect: () => setDialog({ kind: 'histogram', column }) },
            { label: 'Scatter Plot…', onSelect: () => setDialog({ kind: 'scatter', column }) },
            { label: 'Group By…', onSelect: () => setDialog({ kind: 'groupBy', column }) },
            'separator',
            { label: 'Format…', onSelect: () => setDialog({ kind: 'format', column }) },
            { label: 'Hide Column', onSelect: () => hideColumn(column) },
        );
        if (layout.hidden.size > 0)
            items.push({
                label: `Show ${layout.hidden.size} Hidden Column${layout.hidden.size > 1 ? 's' : ''}`,
                onSelect: () => setLayout({ ...layout, hidden: new Set() }),
            });
        items.push({ label: 'Freeze Up to This Column', onSelect: () => setLayout({ ...layout, frozen: column + 1 }) });
        if (layout.frozen > 0)
            items.push({ label: 'Unfreeze Columns', onSelect: () => setLayout({ ...layout, frozen: 0 }) });
        return items;
    };

    const contextMenu = (target: ContextTarget) => {
        const current = flow.current.info;
        if (!current) return;
        const items: MenuItem[] = [];
        if (target.row !== null) {
            const cell = { row: target.row, column: target.column };
            items.push({ label: 'Copy', onSelect: () => grid.current && void copy(grid.current.selection()) });
            if (current.capabilities.filter)
                items.push(
                    { label: 'Filter: Equal to This Value', onSelect: () => void cellFilter(cell, true) },
                    { label: 'Filter: Not Equal to This Value', onSelect: () => void cellFilter(cell, false) },
                );
            items.push('separator');
        }
        setMenu({ x: target.x, y: target.y, items: [...items, ...columnMenu(target.column)] });
    };

    const showCell = (cell: Cell, range: Range) => {
        const name = flow.current.info?.columns[cell.column]?.name ?? '';
        let text = `Row ${(cell.row + 1).toLocaleString()}, ${name}`;
        if (range.bottom > range.top || range.right > range.left)
            text += ` (${(range.bottom - range.top + 1).toLocaleString()} × ${range.right - range.left + 1} selected)`;
        setCellStatus(text);
        setCursorColumn(cell.column);
    };

    const copy = async (range: Range) => {
        const current = flow.current.info;
        if (!current) return;
        if (range.bottom - range.top + 1 > MAX_COPY_ROWS) {
            setMessage(`Select at most ${MAX_COPY_ROWS.toLocaleString()} rows to copy.`);
            return;
        }
        try {
            const copied = await client.call('copy', { sheet: current.sheet, ...range, hidden: [...layout.hidden] });
            await navigator.clipboard.writeText(copied.text);
            setCellStatus(
                copied.skippedRows > 0
                    ? `Copied ${copied.copiedRows} rows; ${copied.skippedRows} not loaded yet were left out`
                    : `Copied ${copied.copiedRows.toLocaleString()} rows`,
            );
        } catch (error) {
            setMessage(`Cannot copy: ${errorText(error)}`);
        }
    };

    // Continues reading past a memory checkpoint, or ends the table there.
    const readOn = async (go: boolean) => {
        const current = flow.current.info;
        if (!current) return;
        try {
            await client.call(go ? 'continueReading' : 'stopReading', { sheet: current.sheet });
            await refreshInfo();
        } catch (error) {
            setMessage(errorText(error));
        }
    };

    const fetchRows = useCallback(
        (first: number, last: number, firstColumn: number, lastColumn: number) => {
            const current = flow.current.info;
            if (!current) return Promise.reject(new Error('no sheet'));
            const epoch = flow.current.epoch;
            return client.call('rows', { sheet: current.sheet, first, last, firstColumn, lastColumn }).then((rows) => {
                if (epoch !== flow.current.epoch) throw new Error('stale');
                return rows;
            });
        },
        [client],
    );

    // Start: the engine, then the filter operators it offers.
    useEffect(() => {
        if (!ISOLATED) return;
        const unsubscribe = client.subscribeErrors((text) =>
            setMessage(`The engine stopped: ${text}. Reload the page to start again.`),
        );
        let cancelled = false;
        client.ready
            .then(() => client.call('filterOps', {}))
            .then(
                (ops) => {
                    if (cancelled) return;
                    setFilterOps(ops);
                    setPhase((current) => (current.kind === 'starting' ? { kind: 'idle' } : current));
                    document.body.dataset.ready = 'true';
                },
                (error: unknown) => setMessage(errorText(error)),
            );
        return () => {
            cancelled = true;
            unsubscribe();
        };
    }, [client]);

    // The model's events: look again at what changed.
    useEffect(
        () =>
            client.subscribe((events) => {
                const current = flow.current.info;
                const bits = current && events.get(current.sheet);
                if (!bits) return;
                if (bits & Events.VIEW) setViewVersion((version) => version + 1);
                else if (bits & Events.ROWS) setDataVersion((version) => version + 1);
                if (bits & (Events.STATE | Events.ROW_COUNT | Events.VIEW | Events.CHECKPOINT)) void refreshInfo();
                if (bits & (Events.SEARCH | Events.VIEW)) void searchProgressed();
            }),
        [client, refreshInfo, searchProgressed],
    );

    // Ctrl+F, Ctrl+G and Ctrl+O, and dropping a file anywhere.
    useEffect(() => {
        let depth = 0;
        const keyDown = (event: KeyboardEvent) => {
            const ctrl = event.ctrlKey || event.metaKey;
            if (ctrl && event.key.toLowerCase() === 'f' && searchInput.current && !searchInput.current.disabled) {
                event.preventDefault();
                searchInput.current.focus();
                searchInput.current.select();
            } else if (ctrl && event.key.toLowerCase() === 'g' && flow.current.info) {
                event.preventDefault();
                setDialog({ kind: 'goTo', column: 0 });
            } else if (ctrl && event.key.toLowerCase() === 'o') {
                event.preventDefault();
                document.getElementById('file-input')?.click();
            }
        };
        const dragEnter = (event: DragEvent) => {
            if (event.dataTransfer?.types.includes('Files')) {
                depth++;
                setDragging(true);
            }
        };
        const dragLeave = () => {
            depth = Math.max(0, depth - 1);
            if (depth === 0) setDragging(false);
        };
        const dragOver = (event: DragEvent) => event.preventDefault();
        const drop = (event: DragEvent) => {
            event.preventDefault();
            depth = 0;
            setDragging(false);
            const dropped = event.dataTransfer?.files[0];
            if (dropped) void openFile(dropped);
        };
        document.addEventListener('keydown', keyDown);
        window.addEventListener('dragenter', dragEnter);
        window.addEventListener('dragleave', dragLeave);
        window.addEventListener('dragover', dragOver);
        window.addEventListener('drop', drop);
        return () => {
            document.removeEventListener('keydown', keyDown);
            window.removeEventListener('dragenter', dragEnter);
            window.removeEventListener('dragleave', dragLeave);
            window.removeEventListener('dragover', dragOver);
            window.removeEventListener('drop', drop);
        };
    }, [openFile]);

    const fileText = !file ? '' : book?.textEncoding ? `${file.name} (${book.textEncoding})` : file.name;
    let rowsText = info
        ? info.rowsText + (info.pending ? ' (applying…)' : '') + (info.readingStopped ? ' (reading stopped)' : '')
        : '';
    if (info && layout.hidden.size > 0)
        rowsText += ` · ${layout.hidden.size} column${layout.hidden.size > 1 ? 's' : ''} hidden`;
    const showGrid = phase.kind === 'open' && info !== null && info.columns.length > 0;
    const hidden = [...layout.hidden];
    const sheetTool = (id: string, label: string, title: string, kind: DialogKind): Tool => ({
        id,
        label,
        title,
        enabled: showGrid,
        onClick: () => setDialog({ kind, column: cursorColumn }),
    });
    const tools: Tool[] = [
        {
            id: 'filter-toggle',
            label: 'Filter',
            title: 'Show the filter conditions',
            enabled: !!info?.capabilities.filter,
            pressed: filterOpen,
            onClick: () => setFilterOpen((open) => !open),
        },
        {
            id: 'stats-toggle',
            label: 'Statistics',
            title: "Statistics of the cursor's column",
            enabled: showGrid,
            pressed: statsOpen,
            onClick: () => setStatsOpen((open) => !open),
        },
        sheetTool('histogram-button', 'Histogram', "A histogram of the cursor's column", 'histogram'),
        sheetTool('scatter-button', 'Scatter', 'Two columns plotted against each other', 'scatter'),
        sheetTool('group-by-button', 'Group By', 'Group the rows into a new sheet', 'groupBy'),
        {
            id: 'sql-toggle',
            label: 'SQL',
            title: 'Run SQL on the file',
            enabled: !!book?.supportsSql,
            pressed: sqlOpen,
            onClick: () => setSqlOpen((open) => !open),
        },
        sheetTool('export-button', 'Export', 'Save the view as CSV, TSV, Excel or Parquet', 'export'),
    ];
    const closeDialog = () => {
        setDialog(null);
        grid.current?.focus();
    };
    const goToRow = (row: number) => {
        setDialog(null);
        grid.current?.select({ row, column: grid.current.cursor().column });
        grid.current?.focus();
    };

    return (
        <div className="flex h-full flex-col">
            <Toolbar
                onOpen={(opened) => void openFile(opened)}
                showHeaderMode={!!book?.usesHeaderOption}
                headerMode={headerMode}
                onHeaderMode={(mode) => file && void openFile(file, mode, true)}
                tools={tools}
                search={{
                    ...search,
                    enabled: !!info?.capabilities.search,
                    status: searchStatus,
                    onChange: changeSearch,
                    onFind: (forward) => void (flow.current.searchActive ? findHit(forward) : startSearch()),
                    onLeave: () => grid.current?.focus(),
                    inputRef: searchInput,
                }}
            />
            {filterOpen && info && (
                <FilterBar
                    columns={info.columns}
                    filter={info.view.filter}
                    ops={filterOps}
                    onChange={(filter) => void setFilter(filter)}
                />
            )}
            <CheckpointBanner
                checkpoint={info?.checkpoint ?? null}
                onContinue={() => void readOn(true)}
                onStop={() => void readOn(false)}
            />
            <MessageBanner message={message} />
            <SheetTabs
                sheets={book?.sheets ?? []}
                current={info?.name ?? null}
                onSelect={(name) => void selectSheet(name)}
            />
            <div className="flex min-h-0 flex-1">
                <main className="relative min-w-0 flex-1">
                    <Grid
                        ref={grid}
                        info={phase.kind === 'open' ? info : null}
                        sheetKey={sheetKey}
                        dataVersion={dataVersion}
                        viewVersion={viewVersion}
                        hidden={layout.hidden}
                        frozen={layout.frozen}
                        fetchRows={fetchRows}
                        onSort={(column) => void sortBy(column)}
                        onSelect={showCell}
                        onCopy={(range) => void copy(range)}
                        onContextMenu={contextMenu}
                    />
                    {!showGrid && <EmptyState title={emptyTitle(phase, info)} />}
                    {dragging && <DropOverlay />}
                </main>
                {statsOpen && showGrid && info && (
                    <StatsPanel
                        client={client}
                        info={info}
                        column={Math.min(cursorColumn, info.columns.length - 1)}
                        onClose={() => setStatsOpen(false)}
                    />
                )}
            </div>
            {sqlOpen && book?.supportsSql && (
                <SqlPanel
                    client={client}
                    onResult={(sheet, sheets, sql) => {
                        flow.current.lastSql = sql;
                        void showResult(sheet, sheets);
                    }}
                    onClose={() => setSqlOpen(false)}
                />
            )}
            <StatusBar
                file={fileText}
                rows={rowsText}
                filter={info?.filterText ? `Filter: ${info.filterText}` : ''}
                cell={cellStatus}
            />
            {menu && <Menu {...menu} onClose={() => setMenu(null)} />}
            {dialog && info && showGrid && (
                <>
                    {dialog.kind === 'histogram' && (
                        <HistogramDialog
                            client={client}
                            info={info}
                            column={dialog.column}
                            onFilter={(conditions) => void addConditions(conditions)}
                            onClose={closeDialog}
                        />
                    )}
                    {dialog.kind === 'scatter' && (
                        <ScatterDialog
                            client={client}
                            info={info}
                            column={dialog.column}
                            hidden={hidden}
                            onFilter={(conditions) => void addConditions(conditions)}
                            onGoToRow={goToRow}
                            onClose={closeDialog}
                        />
                    )}
                    {dialog.kind === 'export' && (
                        <ExportDialog client={client} info={info} hidden={hidden} onClose={closeDialog} />
                    )}
                    {dialog.kind === 'groupBy' && (
                        <GroupByDialog
                            client={client}
                            info={info}
                            column={dialog.column}
                            hidden={hidden}
                            onDone={(sheet, sheets) => void showResult(sheet, sheets)}
                            onClose={closeDialog}
                        />
                    )}
                    {dialog.kind === 'format' && (
                        <FormatDialog
                            client={client}
                            info={info}
                            column={dialog.column}
                            onApplied={() => {
                                setDataVersion((version) => version + 1);
                                void refreshInfo();
                            }}
                            onClose={closeDialog}
                        />
                    )}
                    {dialog.kind === 'goTo' && (
                        <GoToDialog
                            client={client}
                            info={info}
                            onGo={(row, column) => {
                                const cursor = grid.current?.cursor() ?? { row: 0, column: 0 };
                                grid.current?.select({ row: row ?? cursor.row, column: column ?? cursor.column });
                            }}
                            onClose={closeDialog}
                        />
                    )}
                </>
            )}
        </div>
    );
}
