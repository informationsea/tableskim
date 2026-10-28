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
    STATS: 1 << 6,
    HISTOGRAM: 1 << 7,
    SCATTER: 1 << 8,
    JOB: 1 << 9, // reported for SESSION
} as const;

/** The "sheet" that session-wide events (JOB) are reported for. */
export const SESSION = 0;

export type HeaderMode = 'auto' | 'yes' | 'no';

export interface BookInfo {
    sheets: string[];
    usesHeaderOption: boolean;
    isTextFormat: boolean;
    supportsSql: boolean;
    textEncoding: string | null;
}

export type DateStyle = 'iso' | 'slash' | 'monthDayYear' | 'dayMonthYear' | 'japanese' | 'monthName';

/** How a column shows its values; copies and exports keep the values. */
export interface ColumnFormat {
    thousands: boolean;
    decimals: number | null; // as stored
    date: DateStyle;
}

export type ColumnKind = 'unknown' | 'number' | 'date' | 'dateTime' | 'boolean' | 'text' | 'mixed';

export interface Column {
    name: string;
    mark: string; // "#" numbers, "@" dates, "?" booleans, "" otherwise
    kind: ColumnKind; // judged from the first rows read
    format: ColumnFormat | null;
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

export interface DateStyleInfo {
    style: DateStyle;
    example: string;
}

/** Conditions a filter request returned, for the view's filter. */
export interface Conditions {
    conditions: Condition[];
}

export interface SqlTable {
    name: string;
    columns: string[];
}

/** Offsets are bytes of the SQL as UTF-8. */
export interface SqlCompletion {
    begin: number;
    end: number;
    common: string; // the start every candidate shares
    candidates: string[];
}

export interface ValueCount {
    value: string;
    count: number;
}

export interface StatsState {
    active: boolean;
    done: boolean;
    column: number;
    scannedRows: number;
    lines: { label: string; value: string }[];
    top: ValueCount[];
}

export type HistogramKind = 'number' | 'time' | 'none';

export interface Histogram {
    kind: HistogramKind;
    summary: string;
    bins: { label: string; range: string; count: number }[];
    top: ValueCount[]; // for kind "none"
}

export interface HistogramState {
    active: boolean;
    done: boolean;
    counting: boolean; // the second pass
    column: number;
    scannedRows: number;
    histogram: Histogram;
}

export interface ScatterAxis {
    kind: HistogramKind;
    ticks: { at: number; label: string }[];
    edges: number[]; // of the density grid's bins
    binRanges: string[];
}

export interface BoxStats {
    count: number;
    min: number;
    q1: number;
    median: number;
    q3: number;
    max: number;
    lowWhisker: number;
    highWhisker: number;
    mean: number;
    outliers: number[];
}

export interface ScatterState {
    active: boolean;
    done: boolean;
    counting: boolean;
    x: number;
    y: number;
    scannedRows: number;
    plottable: boolean;
    summary: string;
    statsText: string;
    pairs: number;
    fit: { slope: number; intercept: number } | null; // y = intercept + slope * x; none for categories
    xAxis: ScatterAxis;
    yAxis: ScatterAxis;
    counts: number[]; // by X bin (or category), then Y bin
    categories: string[]; // a categorical X; empty otherwise
    boxes: BoxStats[];
    boxTexts: string[];
    points: number[]; // x, y, view row, ... (when asked for)
}

export type ExportFormat = 'csv' | 'tsv' | 'excel' | 'parquet';

export interface ExportFormatInfo {
    format: ExportFormat;
    label: string;
    extension: string;
}

/** An export or a Group By, run in the background. */
export type JobState =
    | { kind: null }
    | {
          kind: 'export' | 'groupBy';
          done: boolean;
          rows: number;
          error?: string; // once done: empty if it worked
          file?: string; // export: read it with TableSkimClient.readFile()
          fileName?: string;
          size?: number;
          sheet?: string; // groupBy: the result sheet
          sheets?: string[];
      };

export type GoToReply = { row: number | null; column: number | null } | { error: string };

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
    copy: {
        params: SheetParams & { top: number; left: number; bottom: number; right: number; hidden?: number[] };
        result: CopyReply;
    };
    goTo: { params: SheetParams & { text: string }; result: GoToReply };
    filterOps: { params: Empty; result: FilterOpInfo[] };
    cellFilter: { params: SheetParams & { row: number; column: number; equal: boolean }; result: Conditions };
    setFormat: { params: SheetParams & { column: number } & ColumnFormat; result: Empty };
    dateStyles: { params: Empty; result: DateStyleInfo[] };
    sql: { params: { sql: string }; result: { sheet: string; sheets: string[] } };
    sqlSchema: { params: Empty; result: SqlTable[] };
    completeSql: { params: { sql: string; cursor: number }; result: SqlCompletion };
    startStats: { params: SheetParams & { column: number }; result: StatsState };
    statsState: { params: SheetParams; result: StatsState };
    startHistogram: { params: SheetParams & { column: number; bins?: number }; result: HistogramState };
    histogramState: { params: SheetParams; result: HistogramState };
    histogramFilter: { params: SheetParams & { bin: number }; result: Conditions };
    defaultScatter: {
        params: SheetParams & { column: number; firstRow?: number; lastRow?: number; hidden?: number[] };
        result: { x: number; y: number };
    };
    startScatter: { params: SheetParams & { x: number; y: number; points?: boolean }; result: Empty };
    scatterState: { params: SheetParams & { points?: boolean }; result: ScatterState };
    scatterFilter: {
        params: SheetParams &
            (
                | { xBin: number; yBin: number }
                | { category: number }
                | { xLow: number; xHigh: number; yLow: number; yHigh: number }
            );
        result: Conditions;
    };
    exportFormats: { params: Empty; result: ExportFormatInfo[] };
    startExport: { params: SheetParams & { format: ExportFormat; hidden?: number[] }; result: Empty };
    aggregates: { params: Empty; result: string[] };
    startGroupBy: {
        params: SheetParams & { keys: number[]; values: number[]; aggregates: string[] };
        result: Empty;
    };
    jobState: { params: Empty; result: JobState };
    cancelJob: { params: Empty; result: Empty };
}

export type Method = keyof Methods;
