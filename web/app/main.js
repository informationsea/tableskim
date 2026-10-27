// @ts-check
// The TableSkim web app: one file at a time, read in this browser by the
// core running in a worker.

import { Events, TableSkimClient } from './client.js';
import { VirtualGrid } from './grid.js';

// Copying more rows than this would build a huge string in the worker.
const MAX_COPY_ROWS = 100000;

/**
 * @template {HTMLElement} T
 * @param {string} id
 * @returns {T}
 */
function $(id)
{
    const element = document.getElementById(id);
    if (!element)
        throw new Error(`no #${id}`);
    return /** @type {T} */ (element);
}

const elements = {
    openButton: $('open-button'),
    fileInput: /** @type {HTMLInputElement} */ ($('file-input')),
    headerOption: $('header-option'),
    headerMode: /** @type {HTMLSelectElement} */ ($('header-mode')),
    filterToggle: /** @type {HTMLButtonElement} */ ($('filter-toggle')),
    searchForm: /** @type {HTMLFormElement} */ ($('search-form')),
    searchText: /** @type {HTMLInputElement} */ ($('search-text')),
    searchCase: /** @type {HTMLInputElement} */ ($('search-case')),
    searchRegex: /** @type {HTMLInputElement} */ ($('search-regex')),
    searchPrev: /** @type {HTMLButtonElement} */ ($('search-prev')),
    searchNext: /** @type {HTMLButtonElement} */ ($('search-next')),
    searchStatus: $('search-status'),
    filterBar: $('filter-bar'),
    filterCombine: /** @type {HTMLSelectElement} */ ($('filter-combine')),
    filterConditions: $('filter-conditions'),
    filterForm: /** @type {HTMLFormElement} */ ($('filter-form')),
    filterColumn: /** @type {HTMLSelectElement} */ ($('filter-column')),
    filterOp: /** @type {HTMLSelectElement} */ ($('filter-op')),
    filterValue: /** @type {HTMLInputElement} */ ($('filter-value')),
    filterClear: $('filter-clear'),
    checkpoint: $('checkpoint'),
    checkpointText: $('checkpoint-text'),
    continueReading: $('continue-reading'),
    stopReading: $('stop-reading'),
    message: $('message'),
    tabs: $('tabs'),
    grid: $('grid'),
    empty: $('empty'),
    emptyTitle: $('empty-title'),
    dropOverlay: $('drop-overlay'),
    statusFile: $('status-file'),
    statusRows: $('status-rows'),
    statusFilter: $('status-filter'),
    statusCell: $('status-cell'),
};

const client = new TableSkimClient('tableskim-wasm.js');

/**
 * @typedef {{op: string, label: string, takesValue: boolean}} FilterOpInfo
 * @typedef {{column: number, op: string, value: string}} Condition
 * @typedef {{sort: {column: number, ascending: boolean}[], filter: {combine: string, conditions: Condition[]} | null}} View
 * @typedef {{
 *   sheet: number, name: string, state: string, error?: string, rows: number, rowsExact: boolean,
 *   displayRows: number, rowsText: string, hasHeader: boolean, columns: {name: string, mark: string}[],
 *   capabilities: {sort: boolean, filter: boolean, search: boolean}, view: View, pending: boolean,
 *   filterText: string, viewError?: string, checkpoint: {bytes: number, rows: number} | null,
 *   readingStopped: boolean,
 * }} SheetInfo
 */

const state = {
    /** @type {File | null} */
    file: null,
    /** @type {{sheets: string[], usesHeaderOption: boolean, textEncoding: string | null} | null} */
    book: null,
    /** @type {SheetInfo | null} */
    info: null,
    /** @type {FilterOpInfo[]} */
    filterOps: [],
    // Bumped by every open and sheet change, so that late replies are dropped.
    epoch: 0,
    infoLoading: false,
    infoDirty: false,
    /** @type {{forward: boolean} | null} */
    pendingFind: null,
    searchActive: false,
};

const grid = new VirtualGrid(elements.grid, {
    fetchRows: (first, last, firstColumn, lastColumn) =>
    {
        const info = state.info;
        if (!info)
            return Promise.reject(new Error('no sheet'));
        const epoch = state.epoch;
        return client.call('rows', { sheet: info.sheet, first, last, firstColumn, lastColumn }).then((rows) =>
        {
            if (epoch !== state.epoch)
                throw new Error('stale');
            return rows;
        });
    },
    onSort: (column) => sortBy(column),
    onSelect: (cell, range) => showCell(cell, range),
    onCopy: (range) => copy(range),
});

/** @param {string} text */
function showMessage(text)
{
    elements.message.textContent = text;
    elements.message.hidden = !text;
}

/** @param {unknown} error */
function errorText(error)
{
    return error instanceof Error ? error.message : String(error);
}

/** @param {number} bytes */
function mebibytes(bytes)
{
    return `${Math.round(bytes / 1048576).toLocaleString()} MiB`;
}

/**
 * @param {File} file
 * @param {string} header
 */
async function openFile(file, header = 'auto')
{
    const epoch = ++state.epoch;
    state.file = file;
    state.info = null;
    state.book = null;
    state.searchActive = false;
    grid.clear();
    showMessage('');
    elements.empty.hidden = false;
    elements.emptyTitle.textContent = `Opening ${file.name}…`;
    elements.tabs.hidden = true;
    elements.checkpoint.hidden = true;
    elements.searchStatus.textContent = '';
    document.title = `${file.name} – TableSkim`;
    elements.statusFile.textContent = file.name;
    elements.statusRows.textContent = '';
    elements.statusFilter.textContent = '';
    try
    {
        const book = await client.call('open', { file, header });
        if (epoch !== state.epoch)
            return;
        state.book = book;
        elements.headerOption.hidden = !book.usesHeaderOption;
        elements.headerMode.value = header;
        elements.statusFile.textContent = book.textEncoding ? `${file.name} (${book.textEncoding})` : file.name;
        renderTabs();
        if (book.sheets.length === 0)
            throw new Error('The file has no sheets.');
        await selectSheet(book.sheets[0]);
    }
    catch (error)
    {
        if (epoch !== state.epoch)
            return;
        elements.emptyTitle.textContent = `Cannot open ${file.name}`;
        showMessage(errorText(error));
    }
}

function renderTabs()
{
    const sheets = state.book?.sheets ?? [];
    elements.tabs.replaceChildren(...sheets.map((name) =>
    {
        const button = document.createElement('button');
        button.type = 'button';
        button.textContent = name;
        button.setAttribute('role', 'tab');
        button.setAttribute('aria-selected', String(state.info?.name === name));
        button.addEventListener('click', () => selectSheet(name));
        return button;
    }));
    elements.tabs.hidden = sheets.length < 2;
}

/** @param {string} name */
async function selectSheet(name)
{
    const epoch = ++state.epoch;
    state.searchActive = false;
    state.pendingFind = null;
    elements.searchStatus.textContent = '';
    const info = await client.call('sheet', { name });
    if (epoch !== state.epoch)
        return;
    state.info = info;
    grid.setInfo(info, true);
    renderTabs();
    showInfo(info);
    grid.focus();
}

// Asks for the sheet's state again, one request at a time.
async function refreshInfo()
{
    if (state.infoLoading)
    {
        state.infoDirty = true;
        return;
    }
    const info = state.info;
    if (!info)
        return;
    const epoch = state.epoch;
    state.infoLoading = true;
    try
    {
        do
        {
            state.infoDirty = false;
            const next = await client.call('info', { sheet: info.sheet });
            if (epoch !== state.epoch)
                return;
            state.info = next;
            grid.setInfo(next, false);
            showInfo(next);
        } while (state.infoDirty);
    }
    catch (error)
    {
        showMessage(errorText(error));
    }
    finally
    {
        state.infoLoading = false;
    }
}

/** @param {SheetInfo} info */
function showInfo(info)
{
    const ready = info.state !== 'opening';
    elements.empty.hidden = info.columns.length > 0;
    if (!ready)
        elements.emptyTitle.textContent = `Opening ${info.name}…`;
    else if (info.columns.length === 0 && info.state !== 'error')
        elements.emptyTitle.textContent = `${info.name} is empty`;
    if (info.state === 'error')
    {
        elements.emptyTitle.textContent = `Cannot read ${info.name}`;
        showMessage(info.error ?? 'The sheet cannot be read.');
    }
    if (info.viewError)
        showMessage(`The sort or filter failed: ${info.viewError}`);
    elements.statusRows.textContent = info.rowsText + (info.pending ? ' (applying…)' : '') +
        (info.readingStopped ? ' (reading stopped)' : '');
    elements.statusFilter.textContent = info.filterText ? `Filter: ${info.filterText}` : '';
    elements.filterToggle.disabled = !info.capabilities.filter;
    elements.searchText.disabled = !info.capabilities.search;
    elements.searchPrev.disabled = elements.searchNext.disabled = !info.capabilities.search;
    if (info.checkpoint)
    {
        elements.checkpointText.textContent = `Reading paused after ${info.checkpoint.rows.toLocaleString()} rows ` +
            `(${mebibytes(info.checkpoint.bytes)} in memory).`;
        elements.checkpoint.hidden = false;
    }
    else
        elements.checkpoint.hidden = true;
    renderFilterBar(info);
}

/**
 * @param {{row: number, column: number}} cell
 * @param {{top: number, left: number, bottom: number, right: number}} range
 */
function showCell(cell, range)
{
    const name = state.info?.columns[cell.column]?.name ?? '';
    let text = `Row ${(cell.row + 1).toLocaleString()}, ${name}`;
    if (range.bottom > range.top || range.right > range.left)
        text += ` (${(range.bottom - range.top + 1).toLocaleString()} × ${range.right - range.left + 1} selected)`;
    elements.statusCell.textContent = text;
}

/** @param {number} column */
async function sortBy(column)
{
    const info = state.info;
    if (!info?.capabilities.sort)
        return;
    try
    {
        const reply = await client.call('sortCycle', { sheet: info.sheet, column });
        if (state.info?.sheet === info.sheet)
        {
            state.info.view = reply.view;
            grid.setInfo(state.info, false);
            refreshInfo();
        }
    }
    catch (error)
    {
        showMessage(errorText(error));
    }
}

/**
 * Applies a filter with the current sort.
 * @param {{combine: string, conditions: Condition[]} | null} filter
 */
async function setFilter(filter)
{
    const info = state.info;
    if (!info)
        return;
    try
    {
        await client.call('setView', { sheet: info.sheet, sort: info.view.sort, filter });
        showMessage('');
        refreshInfo();
    }
    catch (error)
    {
        showMessage(errorText(error));
    }
}

/** @param {SheetInfo} info */
function renderFilterBar(info)
{
    const conditions = info.view.filter?.conditions ?? [];
    elements.filterCombine.value = info.view.filter?.combine ?? 'and';
    elements.filterConditions.replaceChildren(...conditions.map((condition, index) =>
    {
        const op = state.filterOps.find((entry) => entry.op === condition.op);
        const item = document.createElement('li');
        const label = document.createElement('span');
        label.textContent = `${info.columns[condition.column]?.name ?? condition.column} ${op?.label ?? condition.op}` +
            (op?.takesValue ? ` "${condition.value}"` : '');
        const remove = document.createElement('button');
        remove.type = 'button';
        remove.textContent = '×';
        remove.title = 'Remove this condition';
        remove.addEventListener('click', () =>
        {
            const rest = conditions.filter((_, i) => i !== index);
            setFilter(rest.length ? { combine: elements.filterCombine.value, conditions: rest } : null);
        });
        item.append(label, remove);
        return item;
    }));
    const selected = elements.filterColumn.value;
    elements.filterColumn.replaceChildren(...info.columns.map((column, index) => new Option(column.name, String(index))));
    if (selected && Number(selected) < info.columns.length)
        elements.filterColumn.value = selected;
    elements.filterToggle.setAttribute('aria-pressed', String(!elements.filterBar.hidden));
    if (conditions.length > 0 && elements.filterBar.hidden && info.capabilities.filter)
        toggleFilterBar(true);
}

/** @param {boolean} show */
function toggleFilterBar(show)
{
    elements.filterBar.hidden = !show;
    elements.filterToggle.setAttribute('aria-pressed', String(show));
    if (show)
        elements.filterValue.focus();
}

function updateFilterValueField()
{
    const op = state.filterOps.find((entry) => entry.op === elements.filterOp.value);
    elements.filterValue.disabled = !op?.takesValue;
}

async function startSearch()
{
    const info = state.info;
    const text = elements.searchText.value;
    if (!info || !text)
        return;
    try
    {
        await client.call('search', {
            sheet: info.sheet,
            text,
            caseSensitive: elements.searchCase.checked,
            regex: elements.searchRegex.checked,
        });
        state.searchActive = true;
        await findHit(true, true);
    }
    catch (error)
    {
        elements.searchStatus.textContent = errorText(error);
    }
}

/**
 * Moves to the next (or previous) hit from the cursor.
 * @param {boolean} forward
 * @param {boolean} [includeFrom] whether a hit at the cursor counts
 */
async function findHit(forward, includeFrom = false)
{
    const info = state.info;
    if (!info || !state.searchActive)
        return;
    const cursor = grid.cursor;
    const hit = await client.call('findHit', { sheet: info.sheet, row: cursor.row, column: cursor.column, forward, includeFrom });
    if (hit.index !== null)
    {
        state.pendingFind = null;
        grid.select({ row: hit.row, column: hit.column });
    }
    else if (hit.pending)
        state.pendingFind = { forward }; // tried again as the search goes on
    await showSearchState();
}

async function showSearchState()
{
    const info = state.info;
    if (!info || !state.searchActive)
        return;
    const search = await client.call('searchState', { sheet: info.sheet });
    let text;
    if (search.error)
        text = search.error;
    else if (!search.done)
        text = `${search.hits.toLocaleString()} hits so far…`;
    else if (search.hits === 0)
        text = 'No hits';
    else
        text = `${search.hits.toLocaleString()}${search.truncated ? '+' : ''} hits`;
    elements.searchStatus.textContent = text;
    if (state.pendingFind && (search.hits > 0 || search.done))
    {
        const { forward } = state.pendingFind;
        state.pendingFind = null;
        if (search.hits > 0)
            await findHit(forward, true);
    }
}

/** @param {{top: number, left: number, bottom: number, right: number}} range */
async function copy(range)
{
    const info = state.info;
    if (!info)
        return;
    if (range.bottom - range.top + 1 > MAX_COPY_ROWS)
    {
        showMessage(`Select at most ${MAX_COPY_ROWS.toLocaleString()} rows to copy.`);
        return;
    }
    try
    {
        const copied = await client.call('copy', { sheet: info.sheet, ...range });
        await navigator.clipboard.writeText(copied.text);
        elements.statusCell.textContent = copied.skippedRows > 0
            ? `Copied ${copied.copiedRows} rows; ${copied.skippedRows} not loaded yet were left out`
            : `Copied ${copied.copiedRows.toLocaleString()} rows`;
    }
    catch (error)
    {
        showMessage(`Cannot copy: ${errorText(error)}`);
    }
}

client.subscribe((events) =>
{
    const info = state.info;
    if (!info)
        return;
    const bits = events.get(info.sheet);
    if (!bits)
        return;
    if (bits & Events.VIEW)
        grid.viewChanged();
    else if (bits & Events.ROWS)
        grid.dataChanged();
    if (bits & (Events.STATE | Events.ROW_COUNT | Events.VIEW | Events.CHECKPOINT))
        refreshInfo();
    if (bits & (Events.SEARCH | Events.VIEW))
        showSearchState();
});

client.onFatalError = (message) =>
{
    showMessage(`The engine stopped: ${message}. Reload the page to start again.`);
};

elements.openButton.addEventListener('click', () => elements.fileInput.click());
elements.fileInput.addEventListener('change', () =>
{
    const file = elements.fileInput.files?.[0];
    if (file)
        openFile(file);
    elements.fileInput.value = '';
});
elements.headerMode.addEventListener('change', () =>
{
    if (state.file)
        openFile(state.file, elements.headerMode.value);
});
elements.filterToggle.addEventListener('click', () => toggleFilterBar(elements.filterBar.hidden));
elements.filterOp.addEventListener('change', updateFilterValueField);
elements.filterForm.addEventListener('submit', (event) =>
{
    event.preventDefault();
    const info = state.info;
    if (!info)
        return;
    const conditions = [...(info.view.filter?.conditions ?? [])];
    conditions.push({ column: Number(elements.filterColumn.value), op: elements.filterOp.value, value: elements.filterValue.value });
    setFilter({ combine: elements.filterCombine.value, conditions });
});
elements.filterCombine.addEventListener('change', () =>
{
    const conditions = state.info?.view.filter?.conditions ?? [];
    if (conditions.length > 0)
        setFilter({ combine: elements.filterCombine.value, conditions });
});
elements.filterClear.addEventListener('click', () => setFilter(null));
elements.searchForm.addEventListener('submit', (event) => event.preventDefault());
elements.searchText.addEventListener('keydown', (event) =>
{
    if (event.key === 'Enter')
    {
        event.preventDefault();
        if (!state.searchActive)
            startSearch();
        else
            findHit(!event.shiftKey);
    }
    else if (event.key === 'Escape')
        grid.focus();
});
// A changed query or option is a new search.
for (const input of [elements.searchText, elements.searchCase, elements.searchRegex])
{
    input.addEventListener(input === elements.searchText ? 'input' : 'change', () =>
    {
        const info = state.info;
        if (state.searchActive && info)
            client.call('cancelSearch', { sheet: info.sheet }).catch(() => {});
        state.searchActive = false;
        state.pendingFind = null;
        elements.searchStatus.textContent = '';
    });
}
elements.searchNext.addEventListener('click', () => (state.searchActive ? findHit(true) : startSearch()));
elements.searchPrev.addEventListener('click', () => (state.searchActive ? findHit(false) : startSearch()));
elements.continueReading.addEventListener('click', () =>
{
    if (state.info)
        client.call('continueReading', { sheet: state.info.sheet }).then(refreshInfo);
});
elements.stopReading.addEventListener('click', () =>
{
    if (state.info)
        client.call('stopReading', { sheet: state.info.sheet }).then(refreshInfo);
});
document.addEventListener('keydown', (event) =>
{
    if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'f' && !elements.searchText.disabled)
    {
        event.preventDefault();
        elements.searchText.focus();
        elements.searchText.select();
    }
    else if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'o')
    {
        event.preventDefault();
        elements.fileInput.click();
    }
});

// Dropping a file anywhere opens it.
let dragDepth = 0;
window.addEventListener('dragenter', (event) =>
{
    if (event.dataTransfer?.types.includes('Files'))
    {
        dragDepth++;
        elements.dropOverlay.hidden = false;
    }
});
window.addEventListener('dragleave', () =>
{
    dragDepth = Math.max(0, dragDepth - 1);
    if (dragDepth === 0)
        elements.dropOverlay.hidden = true;
});
window.addEventListener('dragover', (event) => event.preventDefault());
window.addEventListener('drop', (event) =>
{
    event.preventDefault();
    dragDepth = 0;
    elements.dropOverlay.hidden = true;
    const file = event.dataTransfer?.files[0];
    if (file)
        openFile(file);
});

(async () =>
{
    if (!self.crossOriginIsolated)
    {
        elements.emptyTitle.textContent = 'This page must be served cross-origin isolated';
        showMessage('The engine needs SharedArrayBuffer: serve the page with the headers ' +
            'Cross-Origin-Opener-Policy: same-origin and Cross-Origin-Embedder-Policy: require-corp.');
        return;
    }
    await client.ready;
    state.filterOps = await client.call('filterOps');
    elements.filterOp.replaceChildren(...state.filterOps.map((op) => new Option(op.label, op.op)));
    updateFilterValueField();
    elements.emptyTitle.textContent = 'Open a file, or drop one here';
    document.body.dataset.ready = 'true';
})();
