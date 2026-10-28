interface StatusBarProps {
    file: string;
    rows: string;
    filter: string;
    cell: string;
}

export function StatusBar({ file, rows, filter, cell }: StatusBarProps) {
    return (
        <footer className="flex items-center gap-4 overflow-hidden border-t border-line bg-chrome px-2.5 py-0.5 whitespace-nowrap max-sm:gap-2">
            <span id="status-file">{file}</span>
            <span id="status-rows">{rows}</span>
            <span id="status-filter" className="min-w-0 truncate text-muted">
                {filter}
            </span>
            <span className="flex-1" />
            <span id="status-cell" className="text-muted">
                {cell}
            </span>
        </footer>
    );
}
