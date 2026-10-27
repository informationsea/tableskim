// @ts-check
// A virtual grid: only the cells in view are in the DOM. Everything is
// drawn in a layer that sticks to the viewport, over a spacer as large as
// the whole table, so that the browser scrolls and the grid redraws.

const ROW_HEIGHT = 24;
const HEADER_HEIGHT = 28;
const MIN_WIDTH = 48;
const MAX_WIDTH = 360;
// Browsers cap the size of an element; past this many pixels the scroll
// position is scaled to the rows.
const MAX_SCROLL_HEIGHT = 8_000_000;
// Rows and columns fetched around the ones in view.
const ROW_MARGIN = 40;
const COLUMN_MARGIN = 4;

/**
 * @typedef {{name: string, mark: string}} Column
 * @typedef {{column: number, ascending: boolean}} SortKey
 * @typedef {{row: number, column: number}} Cell
 * @typedef {{top: number, left: number, bottom: number, right: number}} Range
 * @typedef {{first: number, firstColumn: number, rows: string[][], kinds: string[], loading: boolean, end: boolean}} Rows
 * @typedef {{
 *   fetchRows: (first: number, last: number, firstColumn: number, lastColumn: number) => Promise<Rows>,
 *   onSort: (column: number) => void,
 *   onSelect: (cell: Cell, range: Range) => void,
 *   onCopy: (range: Range) => void,
 * }} GridCallbacks
 */

/** @param {string} text */
function escapeHtml(text)
{
    return text.replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c] ?? c);
}

export class VirtualGrid
{
    /**
     * @param {HTMLElement} container
     * @param {GridCallbacks} callbacks
     */
    constructor(container, callbacks)
    {
        this.callbacks = callbacks;
        this.scroller = document.createElement('div');
        this.scroller.className = 'grid-scroller';
        this.scroller.tabIndex = 0;
        this.layer = document.createElement('div');
        this.layer.className = 'grid-layer';
        this.spacer = document.createElement('div');
        this.spacer.className = 'grid-spacer';
        this.scroller.append(this.layer, this.spacer);
        container.append(this.scroller);
        this.measureContext = document.createElement('canvas').getContext('2d');

        /** @type {Column[]} */
        this.columns = [];
        /** @type {number[]} */
        this.widths = [];
        /** @type {SortKey[]} */
        this.sort = [];
        /** @type {Set<number>} */
        this.filteredColumns = new Set();
        this.rowCount = 0;
        this.rowNumberWidth = 56;
        /** Cached rows: row -> texts and kinds from firstColumn on. @type {Map<number, {firstColumn: number, texts: string[], kinds: string, generation: number}>} */
        this.cache = new Map();
        this.generation = 0; // bumped when cached rows may be out of date
        this.fetching = false;
        this.widthsMeasured = false;
        /** @type {Cell} */
        this.cursor = { row: 0, column: 0 };
        /** @type {Cell} */
        this.anchor = { row: 0, column: 0 };
        this.frame = 0;
        // Set by Ctrl+End while the row count is not known yet: the cursor
        // follows the last row as more are read.
        this.followEnd = false;

        this.scroller.addEventListener('scroll', () => this.schedule());
        new ResizeObserver(() => this.schedule()).observe(this.scroller);
        this.layer.addEventListener('mousedown', (event) => this.mouseDown(event));
        this.scroller.addEventListener('keydown', (event) => this.keyDown(event));
    }

    /**
     * A sheet to show, or another state of it (the view, the row count).
     * @param {{columns: Column[], displayRows: number, rows: number, rowsExact: boolean, view: {sort: SortKey[], filter: {conditions: {column: number}[]} | null}}} info
     * @param {boolean} newSheet whether this is another sheet (or file)
     */
    setInfo(info, newSheet)
    {
        const columnsChanged = newSheet || info.columns.length !== this.columns.length ||
            info.columns.some((column, i) => column.name !== this.columns[i]?.name);
        this.columns = info.columns;
        this.rowCount = info.displayRows;
        this.sort = info.view.sort;
        this.filteredColumns = new Set((info.view.filter?.conditions ?? []).map((condition) => condition.column));
        if (columnsChanged)
        {
            this.widths = this.columns.map((column) => this.fitWidth(`${column.mark} ${column.name} ▲`, true));
            this.widthsMeasured = false;
        }
        if (newSheet)
        {
            this.cache.clear();
            this.cursor = { row: 0, column: 0 };
            this.anchor = { row: 0, column: 0 };
            this.scroller.scrollTo(0, 0);
        }
        this.rowNumberWidth = Math.max(56, this.fitWidth(String(this.rowCount), false));
        this.updateExtent();
        if (newSheet)
            this.followEnd = false;
        else if (this.followEnd)
        {
            const last = Math.max(0, info.rows - 1);
            if (this.cursor.row !== last)
                this.select({ row: last, column: this.cursor.column }, this.cursor.row !== this.anchor.row);
            this.followEnd = !info.rowsExact;
        }
        this.schedule();
    }

    /** The rows in the cache may be out of date (rows loaded, view changed). */
    dataChanged()
    {
        this.generation++;
        this.schedule();
    }

    /** Forget the cached rows: the view changed what each row is. */
    viewChanged()
    {
        this.cache.clear();
        this.dataChanged();
    }

    clear()
    {
        this.columns = [];
        this.rowCount = 0;
        this.cache.clear();
        this.schedule();
    }

    focus()
    {
        this.scroller.focus();
    }

    /** @returns {Range} */
    selection()
    {
        return {
            top: Math.min(this.cursor.row, this.anchor.row),
            bottom: Math.max(this.cursor.row, this.anchor.row),
            left: Math.min(this.cursor.column, this.anchor.column),
            right: Math.max(this.cursor.column, this.anchor.column),
        };
    }

    /**
     * Moves the cursor (and, unless extending, the anchor) and scrolls to it.
     * @param {Cell} cell
     * @param {boolean} [extend]
     */
    select(cell, extend = false)
    {
        const row = Math.max(0, Math.min(cell.row, Math.max(0, this.rowCount - 1)));
        const column = Math.max(0, Math.min(cell.column, Math.max(0, this.columns.length - 1)));
        this.cursor = { row, column };
        if (!extend)
            this.anchor = { row, column };
        this.followEnd = false;
        this.scrollToCell(this.cursor);
        this.callbacks.onSelect(this.cursor, this.selection());
        this.schedule();
    }

    /** @param {Cell} cell */
    scrollToCell(cell)
    {
        const view = this.viewport();
        const bodyHeight = view.height - HEADER_HEIGHT;
        if (cell.row < view.firstRow + 1 || cell.row >= view.firstRow + Math.floor(bodyHeight / ROW_HEIGHT) - 1)
        {
            const target = cell.row < view.firstRow + 1 ? cell.row : cell.row - Math.floor(bodyHeight / ROW_HEIGHT) + 2;
            this.scroller.scrollTop = this.rowToScrollTop(Math.max(0, target));
        }
        let x = 0;
        for (let i = 0; i < cell.column; i++)
            x += this.widths[i];
        const visibleWidth = view.width - this.rowNumberWidth;
        if (x < this.scroller.scrollLeft)
            this.scroller.scrollLeft = x;
        else if (x + this.widths[cell.column] > this.scroller.scrollLeft + visibleWidth)
            this.scroller.scrollLeft = x + this.widths[cell.column] - visibleWidth;
    }

    /**
     * @param {string} text
     * @param {boolean} header
     */
    fitWidth(text, header)
    {
        const context = this.measureContext;
        if (!context)
            return 100;
        context.font = header ? '600 13px system-ui, sans-serif' : '13px system-ui, sans-serif';
        const width = Math.ceil(context.measureText(text).width) + 20;
        return Math.max(MIN_WIDTH, Math.min(MAX_WIDTH, width));
    }

    contentHeight()
    {
        return Math.min(this.rowCount * ROW_HEIGHT, MAX_SCROLL_HEIGHT);
    }

    // The scroll range and the rows' range differ only for huge tables.
    scale()
    {
        const body = this.scroller.clientHeight - HEADER_HEIGHT;
        const rowsRange = Math.max(0, this.rowCount * ROW_HEIGHT - body);
        const scrollRange = Math.max(0, this.contentHeight() - body);
        return scrollRange > 0 ? rowsRange / scrollRange : 1;
    }

    /** @param {number} row */
    rowToScrollTop(row)
    {
        return row * ROW_HEIGHT / this.scale();
    }

    viewport()
    {
        const width = this.scroller.clientWidth;
        const height = this.scroller.clientHeight;
        const offset = this.scroller.scrollTop * this.scale(); // pixels into the rows
        const firstRow = Math.floor(offset / ROW_HEIGHT);
        const rowShift = offset - firstRow * ROW_HEIGHT;
        const lastRow = Math.min(this.rowCount - 1, firstRow + Math.ceil((height - HEADER_HEIGHT + rowShift) / ROW_HEIGHT));
        let x = 0;
        let firstColumn = 0;
        while (firstColumn < this.columns.length - 1 && x + this.widths[firstColumn] <= this.scroller.scrollLeft)
            x += this.widths[firstColumn++];
        const columnShift = this.scroller.scrollLeft - x;
        let lastColumn = firstColumn;
        let right = this.widths[firstColumn] - columnShift;
        while (lastColumn < this.columns.length - 1 && right < width - this.rowNumberWidth)
            right += this.widths[++lastColumn];
        return { width, height, firstRow, lastRow, rowShift, firstColumn, lastColumn, columnShift };
    }

    schedule()
    {
        if (!this.frame)
            this.frame = requestAnimationFrame(() =>
            {
                this.frame = 0;
                this.render();
            });
    }

    // Sizes the scrollable area to the table; done before scrolling to a
    // row, which the browser would otherwise clamp to the old size.
    updateExtent()
    {
        const totalWidth = this.rowNumberWidth + this.widths.reduce((sum, width) => sum + width, 0);
        const height = this.scroller.clientHeight;
        this.spacer.style.width = `${totalWidth}px`;
        this.spacer.style.height = `${HEADER_HEIGHT + this.contentHeight()}px`;
        this.spacer.style.marginTop = `${-height}px`;
        this.layer.style.width = `${this.scroller.clientWidth}px`;
        this.layer.style.height = `${height}px`;
    }

    render()
    {
        this.updateExtent();
        const view = this.viewport();
        if (this.columns.length === 0)
        {
            this.layer.innerHTML = '';
            return;
        }
        this.fetchIfNeeded(view);

        const selection = this.selection();
        const parts = [];
        // Cells, then row numbers and headers over them.
        let x = this.rowNumberWidth - view.columnShift;
        for (let column = view.firstColumn; column <= view.lastColumn; column++)
        {
            const width = this.widths[column];
            for (let row = view.firstRow; row <= view.lastRow; row++)
            {
                const y = HEADER_HEIGHT + (row - view.firstRow) * ROW_HEIGHT - view.rowShift;
                const cached = this.cache.get(row);
                const index = cached ? column - cached.firstColumn : -1;
                const kind = cached && index >= 0 && index < cached.kinds.length ? cached.kinds[index] : 'l';
                const text = kind === 'l' ? '' : cached?.texts[index] ?? '';
                let classes = `cell k-${kind}`;
                if (row >= selection.top && row <= selection.bottom && column >= selection.left && column <= selection.right)
                    classes += ' selected';
                if (row === this.cursor.row && column === this.cursor.column)
                    classes += ' cursor';
                parts.push(`<div class="${classes}" data-row="${row}" data-column="${column}" style="left:${x}px;top:${y}px;width:${width}px">${escapeHtml(text)}</div>`);
            }
            x += width;
        }
        for (let row = view.firstRow; row <= view.lastRow; row++)
        {
            const y = HEADER_HEIGHT + (row - view.firstRow) * ROW_HEIGHT - view.rowShift;
            const active = row >= selection.top && row <= selection.bottom ? ' active' : '';
            parts.push(`<div class="row-number${active}" data-row="${row}" style="top:${y}px;width:${this.rowNumberWidth}px">${row + 1}</div>`);
        }
        x = this.rowNumberWidth - view.columnShift;
        for (let column = view.firstColumn; column <= view.lastColumn; column++)
        {
            const width = this.widths[column];
            const info = this.columns[column];
            const sortIndex = this.sort.findIndex((key) => key.column === column);
            let sortMark = '';
            if (sortIndex >= 0)
                sortMark = (this.sort[sortIndex].ascending ? '▲' : '▼') + (this.sort.length > 1 ? String(sortIndex + 1) : '');
            const active = column >= selection.left && column <= selection.right ? ' active' : '';
            const filtered = this.filteredColumns.has(column) ? ' filtered' : '';
            const mark = info.mark ? `<span class="mark">${escapeHtml(info.mark)}</span>` : '';
            parts.push(`<div class="header${active}${filtered}" data-column="${column}" title="${escapeHtml(info.name)}" style="left:${x}px;width:${width}px">` +
                `${mark}<span class="name">${escapeHtml(info.name)}</span><span class="sort">${sortMark}</span><span class="resize" data-resize="${column}"></span></div>`);
            x += width;
        }
        parts.push(`<div class="corner" style="width:${this.rowNumberWidth}px"></div>`);
        this.layer.innerHTML = parts.join('');
    }

    /** @param {ReturnType<VirtualGrid['viewport']>} view */
    fetchIfNeeded(view)
    {
        if (this.fetching || view.lastRow < view.firstRow)
            return;
        let needed = false;
        for (let row = view.firstRow; row <= view.lastRow && !needed; row++)
        {
            const cached = this.cache.get(row);
            needed = !cached || cached.generation < this.generation || cached.firstColumn > view.firstColumn ||
                cached.firstColumn + cached.kinds.length - 1 < Math.min(view.lastColumn, this.columns.length - 1);
        }
        if (!needed)
            return;
        const first = Math.max(0, view.firstRow - ROW_MARGIN);
        const last = view.lastRow + ROW_MARGIN;
        const firstColumn = Math.max(0, view.firstColumn - COLUMN_MARGIN);
        const lastColumn = Math.min(this.columns.length - 1, view.lastColumn + COLUMN_MARGIN);
        const generation = this.generation;
        this.fetching = true;
        this.callbacks.fetchRows(first, last, firstColumn, lastColumn).then((rows) =>
        {
            this.fetching = false;
            rows.rows.forEach((texts, i) =>
            {
                this.cache.set(rows.first + i, { firstColumn: rows.firstColumn, texts, kinds: rows.kinds[i], generation });
            });
            // Rows past the end are gone (a filter, say).
            if (rows.end)
            {
                for (const row of this.cache.keys())
                {
                    if (row >= rows.first + rows.rows.length)
                        this.cache.delete(row);
                }
            }
            if (this.cache.size > 20000)
                this.trimCache(view);
            if (!this.widthsMeasured && rows.rows.length > 0 && !rows.loading && rows.first === 0)
                this.measureWidths(rows);
            this.schedule();
        }, () =>
        {
            this.fetching = false;
        });
    }

    /** @param {ReturnType<VirtualGrid['viewport']>} view */
    trimCache(view)
    {
        for (const row of this.cache.keys())
        {
            if (row < view.firstRow - 2000 || row > view.lastRow + 2000)
                this.cache.delete(row);
        }
    }

    /**
     * Widens the columns to fit the first rows, once.
     * @param {Rows} rows
     */
    measureWidths(rows)
    {
        this.widthsMeasured = true;
        rows.rows.slice(0, 100).forEach((texts) =>
        {
            texts.forEach((text, i) =>
            {
                const column = rows.firstColumn + i;
                this.widths[column] = Math.max(this.widths[column], this.fitWidth(text, false));
            });
        });
    }

    /** @param {MouseEvent} event */
    mouseDown(event)
    {
        const target = /** @type {HTMLElement} */ (event.target);
        const resize = target.closest('[data-resize]');
        if (resize instanceof HTMLElement)
        {
            event.preventDefault();
            this.startResize(Number(resize.dataset.resize), event.clientX);
            return;
        }
        const header = target.closest('.header');
        if (header instanceof HTMLElement)
        {
            this.callbacks.onSort(Number(header.dataset.column));
            return;
        }
        const cell = target.closest('.cell');
        const rowNumber = target.closest('.row-number');
        if (cell instanceof HTMLElement)
        {
            this.select({ row: Number(cell.dataset.row), column: Number(cell.dataset.column) }, event.shiftKey);
            this.dragSelect();
        }
        else if (rowNumber instanceof HTMLElement)
        {
            const row = Number(rowNumber.dataset.row);
            if (!event.shiftKey)
                this.anchor = { row, column: 0 };
            this.cursor = { row, column: Math.max(0, this.columns.length - 1) };
            this.callbacks.onSelect(this.cursor, this.selection());
            this.schedule();
        }
        this.scroller.focus();
    }

    // Extends the selection while the mouse moves with the button down.
    dragSelect()
    {
        /** @param {MouseEvent} event */
        const move = (event) =>
        {
            const element = document.elementFromPoint(event.clientX, event.clientY);
            const cell = element?.closest('.cell');
            if (cell instanceof HTMLElement && this.layer.contains(cell))
            {
                const row = Number(cell.dataset.row);
                const column = Number(cell.dataset.column);
                if (row !== this.cursor.row || column !== this.cursor.column)
                {
                    this.cursor = { row, column };
                    this.callbacks.onSelect(this.cursor, this.selection());
                    this.schedule();
                }
            }
        };
        const up = () =>
        {
            window.removeEventListener('mousemove', move);
            window.removeEventListener('mouseup', up);
        };
        window.addEventListener('mousemove', move);
        window.addEventListener('mouseup', up);
    }

    /**
     * @param {number} column
     * @param {number} startX
     */
    startResize(column, startX)
    {
        const startWidth = this.widths[column];
        /** @param {MouseEvent} event */
        const move = (event) =>
        {
            this.widths[column] = Math.max(MIN_WIDTH, startWidth + event.clientX - startX);
            this.schedule();
        };
        const up = () =>
        {
            window.removeEventListener('mousemove', move);
            window.removeEventListener('mouseup', up);
        };
        window.addEventListener('mousemove', move);
        window.addEventListener('mouseup', up);
    }

    /** @param {KeyboardEvent} event */
    keyDown(event)
    {
        const page = Math.max(1, Math.floor((this.scroller.clientHeight - HEADER_HEIGHT) / ROW_HEIGHT) - 1);
        const { row, column } = this.cursor;
        const ctrl = event.ctrlKey || event.metaKey;
        /** @type {Cell | null} */
        let next = null;
        switch (event.key)
        {
        case 'ArrowDown': next = { row: ctrl ? this.rowCount - 1 : row + 1, column }; break;
        case 'ArrowUp': next = { row: ctrl ? 0 : row - 1, column }; break;
        case 'ArrowRight': next = { row, column: ctrl ? this.columns.length - 1 : column + 1 }; break;
        case 'ArrowLeft': next = { row, column: ctrl ? 0 : column - 1 }; break;
        case 'PageDown': next = { row: row + page, column }; break;
        case 'PageUp': next = { row: row - page, column }; break;
        case 'Home': next = ctrl ? { row: 0, column: 0 } : { row, column: 0 }; break;
        case 'End': next = ctrl ? { row: this.rowCount - 1, column: this.columns.length - 1 } : { row, column: this.columns.length - 1 }; break;
        case 'c':
        case 'C':
            if (ctrl && !event.shiftKey && !event.altKey)
            {
                event.preventDefault();
                this.callbacks.onCopy(this.selection());
            }
            return;
        case 'a':
        case 'A':
            if (ctrl)
            {
                event.preventDefault();
                this.anchor = { row: 0, column: 0 };
                this.cursor = { row: Math.max(0, this.rowCount - 1), column: Math.max(0, this.columns.length - 1) };
                this.callbacks.onSelect(this.cursor, this.selection());
                this.schedule();
            }
            return;
        default:
            return;
        }
        event.preventDefault();
        this.select(next, event.shiftKey);
        this.followEnd = ctrl && (event.key === 'End' || event.key === 'ArrowDown');
    }
}
