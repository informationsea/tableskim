// The messages between the app and the core in its worker: see
// src/wasm/web_session.cxx and src/wasm/README.md. Rows and columns are
// 0-based; a sheet is the id that a "sheet" request returned.

/** Bits of WebSession::Event: what changed in a sheet. */
export const Events = {
    ROWS: 1 << 0,
    ROW_COUNT: 1 << 1,
    VIEW: 1 << 2,
    SEARCH: 1 << 3,
    CHECKPOINT: 1 << 4,
    STATE: 1 << 5,
} as const;

export type HeaderMode = 'auto' | 'yes' | 'no';

export interface BookInfo {
    sheets: string[];
    usesHeaderOption: boolean;
    isTextFormat: boolean;
    supportsSql: boolean;
    textEncoding: string | null;
}

export interface Column {
    name: string;
    mark: string; // "#" numbers, "@" dates, "?" booleans, "" otherwise
}

export interface SortKey {
    column: number;
    ascending: boolean;
}

export interface Condition {
    column: number;
    op: string;
    value: string;
}

export interface Filter {
    combine: 'and' | 'or';
    conditions: Condition[];
}

export interface View {
    sort: SortKey[];
    filter: Filter | null;
}

export interface SheetInfo {
    sheet: number;
    name: string;
    state: 'opening' | 'ready' | 'error';
    error?: string;
    rows: number;
    rowsExact: boolean;
    displayRows: number; // with some slack while the count is not known
    rowsText: string;
    hasHeader: boolean;
    columns: Column[];
    capabilities: { sort: boolean; filter: boolean; search: boolean };
    view: View;
    pending: boolean; // a view is being applied
    filterText: string;
    viewError?: string; // reported once
    checkpoint: { bytes: number; rows: number } | null;
    readingStopped: boolean;
}

/**
 * One character per cell: n number, s other, e empty, l loading, o beyond
 * the row, x error.
 */
export type CellKind = 'n' | 's' | 'e' | 'l' | 'o' | 'x';

export interface RowsReply {
    first: number;
    firstColumn: number;
    rows: string[][];
    kinds: string[];
    loading: boolean;
    end: boolean; // the table ends after these rows
}

export interface SearchState {
    active: boolean;
    done: boolean;
    truncated: boolean;
    error: string;
    scannedRows: number;
    hits: number;
    generation: number;
}

export interface HitReply {
    index: number | null;
    row: number;
    column: number;
    hits: number;
    wrapped: boolean;
    pending: boolean; // none found yet, but the search goes on
}

export interface CopyReply {
    text: string;
    copiedRows: number;
    skippedRows: number;
}

export interface FilterOpInfo {
    op: string;
    label: string;
    takesValue: boolean;
}

type Empty = Record<string, never>;
type SheetParams = { sheet: number };

/** Each method's parameters and result. */
export interface Methods {
    open: { params: { file: File; header?: HeaderMode; encoding?: string }; result: BookInfo };
    close: { params: Empty; result: Empty };
    sheet: { params: { name: string }; result: SheetInfo };
    info: { params: SheetParams; result: SheetInfo };
    rows: {
        params: SheetParams & { first: number; last: number; firstColumn?: number; lastColumn?: number };
        result: RowsReply;
    };
    setView: { params: SheetParams & { sort: SortKey[]; filter: Filter | null }; result: { view: View } };
    sortCycle: { params: SheetParams & { column: number }; result: { view: View } };
    search: {
        params: SheetParams & { text: string; caseSensitive: boolean; regex: boolean; column?: number };
        result: SearchState;
    };
    cancelSearch: { params: SheetParams; result: SearchState };
    searchState: { params: SheetParams; result: SearchState };
    findHit: {
        params: SheetParams & { row: number; column: number; forward: boolean; includeFrom?: boolean };
        result: HitReply;
    };
    continueReading: { params: SheetParams; result: Empty };
    stopReading: { params: SheetParams; result: Empty };
    copy: { params: SheetParams & { top: number; left: number; bottom: number; right: number }; result: CopyReply };
    filterOps: { params: Empty; result: FilterOpInfo[] };
}

export type Method = keyof Methods;
