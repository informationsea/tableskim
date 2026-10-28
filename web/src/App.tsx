// The TableSkim web app: one file at a time, read in this browser by the
// core running in a worker.

import { useCallback, useEffect, useRef, useState } from 'react';
import type { TableSkimClient } from './client';
import { CheckpointBanner, MessageBanner } from './components/Banners';
import { DropOverlay, EmptyState } from './components/EmptyState';
import { ExportDialog } from './components/ExportDialog';
import { FilterPane } from './components/FilterPane';
import { FormatDialog } from './components/FormatDialog';
import { GoToDialog } from './components/GoToDialog';
import { GroupByDialog } from './components/GroupByDialog';
import { HistogramDialog } from './components/HistogramDialog';
import { Menu, type MenuItem } from './components/Menu';
import { ScatterDialog } from './components/ScatterDialog';
import { ContentPane } from './components/ContentPane';
import { SheetTabs } from './components/SheetTabs';
import { Sidebar, SIDEBAR_MIN_WIDTH, type SidebarTab } from './components/Sidebar';
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
    type OpenOptions,
    type SheetInfo,
    type SortKey,
} from './protocol';

// Copying more rows than this would build a huge string in the worker.
const MAX_COPY_ROWS = 100000;
// WebSession limits one rows reply to this many columns.
const MAX_ROW_COLUMNS = 500;

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

// View settings kept in this browser, as the GUI keeps them; storage may be unavailable.
const MONOSPACE_KEY = 'tableskim.monospace';
const SIDEBAR_WIDTH_KEY = 'tableskim.sidebarWidth';

function stored(key: string): string | null {
    try {
        return localStorage.getItem(key);
    } catch {
        return null;
    }
}

function store(key: string, value: string) {
    try {
        localStorage.setItem(key, value);
    } catch {
        // Not kept, then.
    }
}

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
    const [fileEpoch, setFileEpoch] = useState(0);
    const [book, setBook] = useState<BookInfo | null>(null);
    const [openOptions, setOpenOptions] = useState<OpenOptions>({});
    // The book last opened from the file, kept while another option fails to
    // open it, so that the options stay to choose another.
    const [optionsBook, setOptionsBook] = useState<BookInfo | null>(null);
    const [info, setInfo] = useState<SheetInfo | null>(null);
    const [sheetKey, setSheetKey] = useState(0);
    const [dataVersion, setDataVersion] = useState(0);
    const [viewVersion, setViewVersion] = useState(0);
    const [message, setMessage] = useState(ISOLATED ? '' : ISOLATION_MESSAGE);
    // A condition the Filter pane is to add ("Filter by This Column…").
    const [filterAdding, setFilterAdding] = useState<{ column: number; serial: number } | null>(null);
    const [cellStatus, setCellStatus] = useState('');
    const [search, setSearch] = useState({ text: '', caseSensitive: false, regex: false });
    const [searchStatus, setSearchStatus] = useState('');
    const [dragging, setDragging] = useState(false);
    const [layout, setLayout] = useState<Layout>(NO_LAYOUT);
    const [cursorColumn, setCursorColumn] = useState(0);
    const [sidebar, setSidebar] = useState<SidebarTab | null>(null); // the tab shown, or closed
    const [sidebarWidth, setSidebarWidth] = useState(() =>
        Math.max(SIDEBAR_MIN_WIDTH, Number(stored(SIDEBAR_WIDTH_KEY)) || 288),
    );
    const [monospace, setMonospace] = useState(() => stored(MONOSPACE_KEY) === 'true');
    const [contentCell, setContentCell] = useState<Cell>({ row: 0, column: 0 });
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
        fileEpoch: 0, // SQL results belong to the file on which they were started
        lastSql: '', // run again after the file is opened again
        viewError: '', // the failure of a view shown last
    });

    const showInfo = useCallback((next: SheetInfo) => {
        flow.current.info = next;
        setInfo(next);
        if (next.state === 'error') setMessage(next.error ?? 'The sheet cannot be read.');
        const viewError = next.viewError ? `${flow.current.fileEpoch}:${next.sheet}:${next.viewErrorId}` : '';
        if (viewError && viewError !== flow.current.viewError) {
            flow.current.viewError = viewError;
            setMessage(`The sort or filter failed: ${next.viewError}`);
        }
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
                setContentCell({ row: 0, column: 0 });
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
        async (opened: File, options: OpenOptions = {}, again = false) => {
            const epoch = ++flow.current.epoch;
            flow.current.fileEpoch++;
            setFileEpoch(flow.current.fileEpoch);
            flow.current.info = null;
            resetSearch();
            setFile(opened);
            setBook(null);
            setInfo(null);
            setOpenOptions(options);
            if (!again) setOptionsBook(null);
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
                const next = await client.call('open', { ...options, file: opened });
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
                setOptionsBook(next);
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
        // The rows are fetched again, without the hits marked.
        if (flow.current.searchActive && current)
            client
                .call('cancelSearch', { sheet: current.sheet })
                .then(() => setDataVersion((version) => version + 1))
                .catch(() => {});
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

    // Changes the view the worker asked for last, so that a sort or filter
    // still being applied is kept.
    const changeView = async (
        change: {
            sort?: SortKey[];
            filter?: Filter | null;
            addConditions?: Condition[];
            addNarrowing?: Condition[];
        },
        quiet = false,
    ): Promise<string | null> => {
        const current = flow.current.info;
        if (!current) return null;
        try {
            const reply = await client.call('setView', { sheet: current.sheet, ...change });
            if (flow.current.info?.sheet === current.sheet) showInfo({ ...flow.current.info, view: reply.view });
            setMessage('');
            void refreshInfo();
            return null;
        } catch (error) {
            // The Filter pane shows its own refusals, as the GUI's does.
            if (!quiet) setMessage(errorText(error));
            return errorText(error);
        }
    };

    const setFilter = (filter: Filter | null) => changeView({ filter }, true);

    // Opens the Filter pane with a new condition for the column.
    const filterByColumn = (column: number) => {
        setSidebar('filter');
        setFilterAdding((adding) => ({ column, serial: (adding?.serial ?? 0) + 1 }));
    };

    // Adds conditions to the view's filter (a histogram's bin, a cell's value...).
    const addConditions = async (conditions: Condition[], narrow = false) => {
        if (!flow.current.info?.capabilities.filter || conditions.length === 0) return;
        await changeView(narrow ? { addNarrowing: conditions } : { addConditions: conditions });
    };

    const setSort = async (sort: SortKey[]) => {
        if (!flow.current.info?.capabilities.sort) return;
        await changeView({ sort });
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
        if (current?.capabilities.filter) {
            items.push({ label: 'Filter by This Column…', onSelect: () => filterByColumn(column) });
            if (current.view.filter)
                items.push({ label: 'Clear Filter', onSelect: () => void changeView({ filter: null }) });
            items.push('separator');
        }
        items.push(
            { label: 'Statistics', onSelect: () => setSidebar('stats') },
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
        setContentCell({ row: range.top, column: range.left });
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
            const requests = [];
            for (let column = firstColumn; column <= lastColumn; column += MAX_ROW_COLUMNS)
                requests.push(
                    client.call('rows', {
                        sheet: current.sheet,
                        first,
                        last,
                        firstColumn: column,
                        lastColumn: Math.min(lastColumn, column + MAX_ROW_COLUMNS - 1),
                    }),
                );
            return Promise.all(requests).then((replies) => {
                if (epoch !== flow.current.epoch) throw new Error('stale');
                const count = Math.min(...replies.map((reply) => reply.rows.length));
                return {
                    first,
                    firstColumn,
                    rows: Array.from({ length: count }, (_, row) => replies.flatMap((reply) => reply.rows[row])),
                    kinds: Array.from({ length: count }, (_, row) => replies.map((reply) => reply.kinds[row]).join('')),
                    hits: replies.some((reply) => reply.hits)
                        ? Array.from({ length: count }, (_, row) => replies.flatMap((reply) => reply.hits?.[row] ?? []))
                        : undefined,
                    loading: replies.some((reply) => reply.loading),
                    end: replies.some((reply) => reply.end),
                };
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
                // A search marks its hits in the rows as it finds them.
                else if (bits & (Events.ROWS | Events.SEARCH)) setDataVersion((version) => version + 1);
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
            if (ctrl && event.shiftKey && event.key.toLowerCase() === 'f') {
                event.preventDefault();
                if (flow.current.info?.capabilities.filter) setSidebar('filter');
            } else if (
                ctrl &&
                event.key.toLowerCase() === 'f' &&
                searchInput.current &&
                !searchInput.current.disabled
            ) {
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
            title: 'The filter conditions, in the sidebar (Ctrl+Shift+F)',
            enabled: !!info?.capabilities.filter,
            pressed: sidebar === 'filter',
            onClick: () => setSidebar((tab) => (tab === 'filter' ? null : 'filter')),
        },
        {
            id: 'stats-toggle',
            label: 'Statistics',
            title: "Statistics of the cursor's column in the sidebar",
            enabled: showGrid,
            pressed: sidebar === 'stats',
            onClick: () => setSidebar((tab) => (tab === 'stats' ? null : 'stats')),
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
        {
            id: 'sidebar-toggle',
            label: 'Sidebar',
            title: 'Show or hide the sidebar: the whole cell (Enter or a double click) and statistics',
            enabled: true,
            pressed: sidebar !== null,
            onClick: () => setSidebar((tab) => (tab ? null : 'content')),
        },
        {
            id: 'monospace-toggle',
            label: 'Monospace',
            title: 'Show the cells in a monospace font',
            enabled: true,
            pressed: monospace,
            onClick: () =>
                setMonospace((on) => {
                    store(MONOSPACE_KEY, String(!on));
                    return !on;
                }),
        },
    ];
    const checkpointControls = {
        checkpoint: info?.checkpoint ?? null,
        onContinue: () => void readOn(true),
        onStop: () => void readOn(false),
    };
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
                book={optionsBook}
                options={openOptions}
                onOptions={(options) => file && void openFile(file, options, true)}
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
                        onOpenCell={() => setSidebar('content')}
                        monospace={monospace}
                    />
                    {!showGrid && <EmptyState title={emptyTitle(phase, info)} />}
                    {dragging && <DropOverlay />}
                </main>
                {sidebar && showGrid && info && (
                    <Sidebar
                        tab={sidebar}
                        onTab={setSidebar}
                        width={sidebarWidth}
                        onWidth={(width) => {
                            setSidebarWidth(width);
                            store(SIDEBAR_WIDTH_KEY, String(width));
                        }}
                        onClose={() => {
                            setSidebar(null);
                            grid.current?.focus();
                        }}
                    >
                        {sidebar === 'content' ? (
                            <ContentPane client={client} info={info} cell={contentCell} monospace={monospace} />
                        ) : sidebar === 'filter' ? (
                            <FilterPane
                                info={info}
                                ops={filterOps}
                                onApply={setFilter}
                                adding={filterAdding}
                                cursorColumn={Math.min(cursorColumn, info.columns.length - 1)}
                            />
                        ) : (
                            <StatsPanel
                                client={client}
                                info={info}
                                column={Math.min(cursorColumn, info.columns.length - 1)}
                            />
                        )}
                    </Sidebar>
                )}
            </div>
            {sqlOpen && book?.supportsSql && (
                <SqlPanel
                    client={client}
                    onResult={(sheet, sheets, sql) => {
                        if (fileEpoch !== flow.current.fileEpoch) return;
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
                            checkpoint={checkpointControls}
                            client={client}
                            info={info}
                            column={dialog.column}
                            onFilter={(conditions) => void addConditions(conditions, true)}
                            onClose={closeDialog}
                        />
                    )}
                    {dialog.kind === 'scatter' && (
                        <ScatterDialog
                            checkpoint={checkpointControls}
                            client={client}
                            info={info}
                            column={dialog.column}
                            hidden={hidden}
                            onFilter={(conditions) => void addConditions(conditions, true)}
                            onGoToRow={goToRow}
                            onClose={closeDialog}
                        />
                    )}
                    {dialog.kind === 'export' && (
                        <ExportDialog
                            checkpoint={checkpointControls}
                            client={client}
                            info={info}
                            hidden={hidden}
                            onClose={closeDialog}
                        />
                    )}
                    {dialog.kind === 'groupBy' && (
                        <GroupByDialog
                            checkpoint={checkpointControls}
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
