export function EmptyState({ title }: { title: string }) {
    return (
        <div id="empty" className="absolute inset-0 z-2 flex flex-col items-center justify-center p-4 text-center">
            <p id="empty-title" className="my-1 max-w-xl text-base font-semibold">
                {title}
            </p>
            <p className="my-1 max-w-xl text-muted">
                CSV, TSV, Excel, Parquet, JSON, SQLite and DuckDB files, compressed or not. The file is read here in
                your browser; it is not uploaded anywhere.
            </p>
        </div>
    );
}

export function DropOverlay() {
    return (
        <div
            id="drop-overlay"
            className="pointer-events-none absolute inset-2 z-5 flex items-center justify-center rounded-lg border-2 border-dashed border-accent bg-selection text-base"
        >
            Drop the file to open it
        </div>
    );
}
