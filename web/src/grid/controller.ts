// The state and behaviour of the virtual grid, apart from drawing it
// (Grid.tsx): which rows and columns are in view, the rows fetched from
// the core, the cursor and selection, the keyboard and the mouse.
//
// Only the cells in view are drawn, in a layer that sticks to the
// viewport, over a spacer as large as the whole table, so that the browser
// scrolls and the grid redraws.

import type { CellKind, Column, RowsReply, SheetInfo, SortKey } from '../protocol';

export const ROW_HEIGHT = 24;
export const HEADER_HEIGHT = 28;
const MIN_WIDTH = 48;
const MAX_WIDTH = 360;
// Browsers cap the size of an element; past this many pixels the scroll
// position is scaled to the rows.
const MAX_SCROLL_HEIGHT = 8_000_000;
// Rows and columns fetched around the ones in view.
const ROW_MARGIN = 40;
const COLUMN_MARGIN = 4;
const MAX_CACHED_ROWS = 20000;

export interface Cell {
    row: number;
    column: number;
}

export interface Range {
    top: number;
    left: number;
    bottom: number;
    right: number;
}

export interface Viewport {
    width: number;
    height: number;
    firstRow: number;
    lastRow: number;
    rowShift: number; // pixels of firstRow above the body
    firstColumn: number;
    lastColumn: number;
    columnShift: number; // pixels of firstColumn left of the body
}

/** A right click: on a header (row null) or a cell. */
export interface ContextTarget {
    row: number | null;
    column: number;
    x: number;
    y: number;
}

export interface GridCallbacks {
    fetchRows: (first: number, last: number, firstColumn: number, lastColumn: number) => Promise<RowsReply>;
    onSort: (column: number) => void;
    onSelect: (cell: Cell, range: Range) => void;
    onCopy: (range: Range) => void;
    onContextMenu: (target: ContextTarget) => void;
}

interface CachedRow {
    firstColumn: number;
    texts: string[];
    kinds: string;
    generation: number;
}

export class GridController {
    columns: Column[] = [];
    widths: number[] = [];
    sort: SortKey[] = [];
    filteredColumns = new Set<number>();
    rowCount = 0;
    rowNumberWidth = 56;
    // Hidden columns are drawn 0 pixels wide; the first `frozen` columns
    // stay in view, right of the row numbers.
    hidden = new Set<number>();
    frozen = 0;
    cursor: Cell = { row: 0, column: 0 };
    anchor: Cell = { row: 0, column: 0 };

    private scroller: HTMLElement | null = null;
    private layer: HTMLElement | null = null;
    private spacer: HTMLElement | null = null;
    private readonly cache = new Map<number, CachedRow>();
    private generation = 0; // bumped when cached rows may be out of date
    private fetching = false;
    private widthsMeasured = false;
    // Set by Ctrl+End while the row count is not known yet: the cursor
    // follows the last row as more are read.
    private followEnd = false;
    private readonly measureContext = document.createElement('canvas').getContext('2d');

    private handlers: GridCallbacks | null = null;
    private frame = 0;

    /** @param draw draws the grid again (at most once per frame, see redraw()) */
    constructor(private readonly draw: () => void) {}

    /** The callbacks of the latest render (they change with renders). */
    setCallbacks(callbacks: GridCallbacks) {
        this.handlers = callbacks;
    }

    private callbacks(): GridCallbacks {
        if (!this.handlers) throw new Error('GridController: no callbacks');
        return this.handlers;
    }

    /** Asks for the grid to be drawn again, once in the next frame. */
    readonly redraw = () => {
        if (!this.frame)
            this.frame = requestAnimationFrame(() => {
                this.frame = 0;
                this.draw();
            });
    };

    detach() {
        cancelAnimationFrame(this.frame);
        this.frame = 0;
        this.attach(null, null, null);
    }

    attach(scroller: HTMLElement | null, layer: HTMLElement | null, spacer: HTMLElement | null) {
        this.scroller = scroller;
        this.layer = layer;
        this.spacer = spacer;
    }

    /** A sheet to show (newSheet), or another state of it: the view, the rows. */
    setInfo(info: SheetInfo, newSheet: boolean) {
        const columnsChanged =
            newSheet ||
            info.columns.length !== this.columns.length ||
            info.columns.some((column, i) => column.name !== this.columns[i]?.name);
        this.columns = info.columns;
        this.rowCount = info.displayRows;
        this.sort = info.view.sort;
        this.filteredColumns = new Set((info.view.filter?.conditions ?? []).map((condition) => condition.column));
        if (columnsChanged) {
            this.widths = this.columns.map((column) => this.fitWidth(`${column.mark} ${column.name} ▲`, true));
            this.widthsMeasured = false;
        }
        if (newSheet) {
            this.cache.clear();
            this.cursor = { row: 0, column: 0 };
            this.anchor = { row: 0, column: 0 };
            this.scroller?.scrollTo(0, 0);
        }
        this.rowNumberWidth = Math.max(56, this.fitWidth(String(this.rowCount), false));
        this.updateExtent();
        if (newSheet) this.followEnd = false;
        else if (this.followEnd) {
            const last = Math.max(0, info.rows - 1);
            if (this.cursor.row !== last)
                this.select({ row: last, column: this.cursor.column }, this.cursor.row !== this.anchor.row);
            this.followEnd = !info.rowsExact;
        }
        this.redraw();
    }

    /** Hidden and frozen columns: part of the view, kept by the app. */
    setLayout(hidden: Set<number>, frozen: number) {
        this.hidden = hidden;
        this.frozen = Math.min(frozen, this.columns.length);
        if (this.hidden.has(this.cursor.column)) {
            const column = this.visibleColumn(this.cursor.column, 1) ?? this.visibleColumn(this.cursor.column, -1);
            if (column !== null) {
                this.cursor = { ...this.cursor, column };
                this.anchor = { ...this.anchor, column };
            }
        }
        this.updateExtent();
        this.redraw();
    }

    /** The column's width as drawn. */
    width(column: number): number {
        return this.hidden.has(column) ? 0 : (this.widths[column] ?? 0);
    }

    frozenWidth(): number {
        let width = 0;
        for (let column = 0; column < this.frozen; column++) width += this.width(column);
        return width;
    }

    // The first column from `column` on (step 1) or back (step -1) that is
    // not hidden.
    private visibleColumn(column: number, step: 1 | -1): number | null {
        for (let c = column; c >= 0 && c < this.columns.length; c += step) if (!this.hidden.has(c)) return c;
        return null;
    }

    /** The cached rows may be out of date (rows loaded, cells changed). */
    dataChanged() {
        this.generation++;
        this.redraw();
    }

    /** Forget the cached rows: the view changed what each row is. */
    viewChanged() {
        this.cache.clear();
        this.dataChanged();
    }

    clear() {
        this.columns = [];
        this.rowCount = 0;
        this.cache.clear();
        this.redraw();
    }

    selection(): Range {
        return {
            top: Math.min(this.cursor.row, this.anchor.row),
            bottom: Math.max(this.cursor.row, this.anchor.row),
            left: Math.min(this.cursor.column, this.anchor.column),
            right: Math.max(this.cursor.column, this.anchor.column),
        };
    }

    /** Moves the cursor (and, unless extending, the anchor) and scrolls to it. */
    select(cell: Cell, extend = false) {
        const row = Math.max(0, Math.min(cell.row, Math.max(0, this.rowCount - 1)));
        let column = Math.max(0, Math.min(cell.column, Math.max(0, this.columns.length - 1)));
        if (this.hidden.has(column))
            column =
                this.visibleColumn(column, cell.column < this.cursor.column ? -1 : 1) ??
                this.visibleColumn(column, cell.column < this.cursor.column ? 1 : -1) ??
                column;
        this.cursor = { row, column };
        if (!extend) this.anchor = { row, column };
        this.followEnd = false;
        this.scrollToCell(this.cursor);
        this.callbacks().onSelect(this.cursor, this.selection());
        this.redraw();
    }

    /** Moves the cursor without scrolling (dragging, row numbers). */
    private moveCursor(cursor: Cell, anchor?: Cell) {
        this.cursor = cursor;
        if (anchor) this.anchor = anchor;
        this.callbacks().onSelect(this.cursor, this.selection());
        this.redraw();
    }

    scrollToCell(cell: Cell) {
        const scroller = this.scroller;
        if (!scroller) return;
        const view = this.viewport();
        const bodyRows = Math.floor((view.height - HEADER_HEIGHT) / ROW_HEIGHT);
        if (cell.row < view.firstRow + 1 || cell.row >= view.firstRow + bodyRows - 1) {
            const target = cell.row < view.firstRow + 1 ? cell.row : cell.row - bodyRows + 2;
            scroller.scrollTop = this.rowToScrollTop(Math.max(0, target));
        }
        if (cell.column < this.frozen) return; // always in view
        let x = 0;
        for (let i = this.frozen; i < cell.column; i++) x += this.width(i);
        const visibleWidth = view.width - this.rowNumberWidth - this.frozenWidth();
        if (x < scroller.scrollLeft) scroller.scrollLeft = x;
        else if (x + this.width(cell.column) > scroller.scrollLeft + visibleWidth)
            scroller.scrollLeft = x + this.width(cell.column) - visibleWidth;
    }

    private fitWidth(text: string, header: boolean): number {
        const context = this.measureContext;
        if (!context) return 100;
        context.font = header ? '600 13px system-ui, sans-serif' : '13px system-ui, sans-serif';
        const width = Math.ceil(context.measureText(text).width) + 20;
        return Math.max(MIN_WIDTH, Math.min(MAX_WIDTH, width));
    }

    private contentHeight(): number {
        return Math.min(this.rowCount * ROW_HEIGHT, MAX_SCROLL_HEIGHT);
    }

    // The scroll range and the rows' range differ only for huge tables.
    private scale(): number {
        const body = (this.scroller?.clientHeight ?? 0) - HEADER_HEIGHT;
        const rowsRange = Math.max(0, this.rowCount * ROW_HEIGHT - body);
        const scrollRange = Math.max(0, this.contentHeight() - body);
        return scrollRange > 0 ? rowsRange / scrollRange : 1;
    }

    private rowToScrollTop(row: number): number {
        return (row * ROW_HEIGHT) / this.scale();
    }

    viewport(): Viewport {
        const scroller = this.scroller;
        const width = scroller?.clientWidth ?? 0;
        const height = scroller?.clientHeight ?? 0;
        const scrollLeft = scroller?.scrollLeft ?? 0;
        const offset = (scroller?.scrollTop ?? 0) * this.scale(); // pixels into the rows
        const firstRow = Math.floor(offset / ROW_HEIGHT);
        const rowShift = offset - firstRow * ROW_HEIGHT;
        const lastRow = Math.min(
            this.rowCount - 1,
            firstRow + Math.ceil((height - HEADER_HEIGHT + rowShift) / ROW_HEIGHT),
        );
        // The columns scrolled past the frozen ones.
        let x = 0;
        let firstColumn = Math.min(this.frozen, Math.max(0, this.columns.length - 1));
        while (firstColumn < this.columns.length - 1 && x + this.width(firstColumn) <= scrollLeft)
            x += this.width(firstColumn++);
        const columnShift = scrollLeft - x;
        let lastColumn = firstColumn;
        let right = this.width(firstColumn) - columnShift;
        const room = width - this.rowNumberWidth - this.frozenWidth();
        while (lastColumn < this.columns.length - 1 && right < room) right += this.width(++lastColumn);
        return { width, height, firstRow, lastRow, rowShift, firstColumn, lastColumn, columnShift };
    }

    /**
     * Sizes the scrollable area to the table; done before scrolling to a
     * row, which the browser would otherwise clamp to the old size. The
     * layer and the spacer are sized here, not by React.
     */
    updateExtent() {
        const scroller = this.scroller;
        if (!scroller || !this.layer || !this.spacer) return;
        let totalWidth = this.rowNumberWidth;
        for (let column = 0; column < this.columns.length; column++) totalWidth += this.width(column);
        const height = scroller.clientHeight;
        this.spacer.style.width = `${totalWidth}px`;
        this.spacer.style.height = `${HEADER_HEIGHT + this.contentHeight()}px`;
        this.spacer.style.marginTop = `${-height}px`;
        this.layer.style.width = `${scroller.clientWidth}px`;
        this.layer.style.height = `${height}px`;
    }

    /** What to draw in a cell. */
    cell(row: number, column: number): { kind: CellKind; text: string } {
        const cached = this.cache.get(row);
        const index = cached ? column - cached.firstColumn : -1;
        const kind = (cached && index >= 0 && index < cached.kinds.length ? cached.kinds[index] : 'l') as CellKind;
        return { kind, text: kind === 'l' ? '' : (cached?.texts[index] ?? '') };
    }

    /** Asks for the rows in view (and some around) unless they are cached. */
    fetchIfNeeded(view: Viewport) {
        if (this.fetching || view.lastRow < view.firstRow || this.columns.length === 0) return;
        // Frozen columns are fetched with the ones in view.
        const leftColumn = this.frozen > 0 ? 0 : view.firstColumn;
        let needed = false;
        for (let row = view.firstRow; row <= view.lastRow && !needed; row++) {
            const cached = this.cache.get(row);
            needed =
                !cached ||
                cached.generation < this.generation ||
                cached.firstColumn > leftColumn ||
                cached.firstColumn + cached.kinds.length - 1 < Math.min(view.lastColumn, this.columns.length - 1);
        }
        if (!needed) return;
        const first = Math.max(0, view.firstRow - ROW_MARGIN);
        const last = view.lastRow + ROW_MARGIN;
        const firstColumn = Math.max(0, leftColumn - COLUMN_MARGIN);
        const lastColumn = Math.min(this.columns.length - 1, view.lastColumn + COLUMN_MARGIN);
        const generation = this.generation;
        this.fetching = true;
        this.callbacks()
            .fetchRows(first, last, firstColumn, lastColumn)
            .then(
                (rows) => {
                    this.fetching = false;
                    rows.rows.forEach((texts, i) => {
                        this.cache.set(rows.first + i, {
                            firstColumn: rows.firstColumn,
                            texts,
                            kinds: rows.kinds[i],
                            generation,
                        });
                    });
                    // Rows past the end are gone (a filter, say).
                    if (rows.end) {
                        for (const row of this.cache.keys()) {
                            if (row >= rows.first + rows.rows.length) this.cache.delete(row);
                        }
                    }
                    if (this.cache.size > MAX_CACHED_ROWS) this.trimCache(view);
                    if (!this.widthsMeasured && rows.rows.length > 0 && !rows.loading && rows.first === 0)
                        this.measureWidths(rows);
                    this.redraw();
                },
                () => {
                    // Dropped (another sheet is shown now) or failed: look again
                    // soon, as no event may come to ask for the rows in view.
                    this.fetching = false;
                    setTimeout(this.redraw, 100);
                },
            );
    }

    private trimCache(view: Viewport) {
        for (const row of this.cache.keys()) {
            if (row < view.firstRow - 2000 || row > view.lastRow + 2000) this.cache.delete(row);
        }
    }

    // Widens the columns to fit the first rows, once.
    private measureWidths(rows: RowsReply) {
        this.widthsMeasured = true;
        for (const texts of rows.rows.slice(0, 100)) {
            texts.forEach((text, i) => {
                const column = rows.firstColumn + i;
                this.widths[column] = Math.max(this.widths[column], this.fitWidth(text, false));
            });
        }
        this.updateExtent();
    }

    mouseDown(event: MouseEvent) {
        if (event.button !== 0) return; // a right click opens a menu: see contextMenu()
        const target = event.target as HTMLElement;
        const resize = target.closest<HTMLElement>('[data-resize]');
        if (resize) {
            event.preventDefault();
            this.startResize(Number(resize.dataset.resize), event.clientX);
            return;
        }
        const header = target.closest<HTMLElement>('.header');
        if (header) {
            this.callbacks().onSort(Number(header.dataset.column));
            return;
        }
        const cell = target.closest<HTMLElement>('.cell');
        const rowNumber = target.closest<HTMLElement>('.row-number');
        if (cell) {
            this.select({ row: Number(cell.dataset.row), column: Number(cell.dataset.column) }, event.shiftKey);
            this.dragSelect();
        } else if (rowNumber) {
            const row = Number(rowNumber.dataset.row);
            this.moveCursor(
                { row, column: Math.max(0, this.columns.length - 1) },
                event.shiftKey ? undefined : { row, column: 0 },
            );
        }
        this.scroller?.focus();
    }

    contextMenu(event: MouseEvent) {
        const target = event.target as HTMLElement;
        const header = target.closest<HTMLElement>('.header');
        const cell = target.closest<HTMLElement>('.cell');
        if (!header && !cell) return;
        event.preventDefault();
        const column = Number((header ?? cell)?.dataset.column);
        let row: number | null = null;
        if (header) {
            // The menu is about the column: the cursor moves there.
            const range = this.selection();
            if (column < range.left || column > range.right) this.select({ row: this.cursor.row, column });
        } else if (cell) {
            row = Number(cell.dataset.row);
            // Inside the selection, the menu is about the selection.
            const range = this.selection();
            if (row < range.top || row > range.bottom || column < range.left || column > range.right)
                this.select({ row, column });
        }
        this.scroller?.focus();
        this.callbacks().onContextMenu({ row, column, x: event.clientX, y: event.clientY });
    }

    // Extends the selection while the mouse moves with the button down.
    private dragSelect() {
        const move = (event: MouseEvent) => {
            const cell = document.elementFromPoint(event.clientX, event.clientY)?.closest<HTMLElement>('.cell');
            if (cell && this.layer?.contains(cell)) {
                const row = Number(cell.dataset.row);
                const column = Number(cell.dataset.column);
                if (row !== this.cursor.row || column !== this.cursor.column) this.moveCursor({ row, column });
            }
        };
        const up = () => {
            window.removeEventListener('mousemove', move);
            window.removeEventListener('mouseup', up);
        };
        window.addEventListener('mousemove', move);
        window.addEventListener('mouseup', up);
    }

    private startResize(column: number, startX: number) {
        const startWidth = this.widths[column];
        const move = (event: MouseEvent) => {
            this.widths[column] = Math.max(MIN_WIDTH, startWidth + event.clientX - startX);
            this.updateExtent();
            this.redraw();
        };
        const up = () => {
            window.removeEventListener('mousemove', move);
            window.removeEventListener('mouseup', up);
        };
        window.addEventListener('mousemove', move);
        window.addEventListener('mouseup', up);
    }

    private firstVisible(): number {
        return this.visibleColumn(0, 1) ?? 0;
    }

    private lastVisible(): number {
        return this.visibleColumn(this.columns.length - 1, -1) ?? 0;
    }

    keyDown(event: KeyboardEvent) {
        const page = Math.max(1, Math.floor(((this.scroller?.clientHeight ?? 0) - HEADER_HEIGHT) / ROW_HEIGHT) - 1);
        const { row, column } = this.cursor;
        const ctrl = event.ctrlKey || event.metaKey;
        const lastRow = this.rowCount - 1;
        const lastColumn = this.columns.length - 1;
        let next: Cell;
        switch (event.key) {
            case 'ArrowDown':
                next = { row: ctrl ? lastRow : row + 1, column };
                break;
            case 'ArrowUp':
                next = { row: ctrl ? 0 : row - 1, column };
                break;
            case 'ArrowRight':
                next = { row, column: (ctrl ? null : this.visibleColumn(column + 1, 1)) ?? this.lastVisible() };
                break;
            case 'ArrowLeft':
                next = { row, column: (ctrl ? null : this.visibleColumn(column - 1, -1)) ?? this.firstVisible() };
                break;
            case 'PageDown':
                next = { row: row + page, column };
                break;
            case 'PageUp':
                next = { row: row - page, column };
                break;
            case 'Home':
                next = { row: ctrl ? 0 : row, column: this.firstVisible() };
                break;
            case 'End':
                next = { row: ctrl ? lastRow : row, column: this.lastVisible() };
                break;
            case 'c':
            case 'C':
                if (ctrl && !event.shiftKey && !event.altKey) {
                    event.preventDefault();
                    this.callbacks().onCopy(this.selection());
                }
                return;
            case 'a':
            case 'A':
                if (ctrl) {
                    event.preventDefault();
                    this.moveCursor(
                        { row: Math.max(0, lastRow), column: Math.max(0, lastColumn) },
                        { row: 0, column: 0 },
                    );
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
