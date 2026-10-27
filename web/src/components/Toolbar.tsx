import { useRef } from 'react';
import type { HeaderMode } from '../protocol';
import { SearchBox, type SearchBoxProps } from './SearchBox';

interface ToolbarProps {
    onOpen: (file: File) => void;
    showHeaderMode: boolean;
    headerMode: HeaderMode;
    onHeaderMode: (mode: HeaderMode) => void;
    filterEnabled: boolean;
    filterOpen: boolean;
    onToggleFilter: () => void;
    search: SearchBoxProps;
}

export function Toolbar({
    onOpen,
    showHeaderMode,
    headerMode,
    onHeaderMode,
    filterEnabled,
    filterOpen,
    onToggleFilter,
    search,
}: ToolbarProps) {
    const input = useRef<HTMLInputElement>(null);
    return (
        <header className="flex flex-wrap items-center gap-2 border-b border-line bg-chrome px-2.5 py-1.5">
            <span className="mr-1 font-bold">TableSkim</span>
            <button type="button" id="open-button" className="control" onClick={() => input.current?.click()}>
                Open…
            </button>
            <input
                type="file"
                id="file-input"
                ref={input}
                hidden
                onChange={(event) => {
                    const file = event.target.files?.[0];
                    if (file) onOpen(file);
                    event.target.value = '';
                }}
            />
            <label id="header-option" className="flex items-center gap-1" hidden={!showHeaderMode}>
                First row
                <select
                    id="header-mode"
                    className="control"
                    value={headerMode}
                    onChange={(event) => onHeaderMode(event.target.value as HeaderMode)}
                >
                    <option value="auto">Auto</option>
                    <option value="yes">Header</option>
                    <option value="no">Data</option>
                </select>
            </label>
            <button
                type="button"
                id="filter-toggle"
                className="control"
                aria-pressed={filterOpen}
                disabled={!filterEnabled}
                onClick={onToggleFilter}
            >
                Filter
            </button>
            <span className="flex-1" />
            <SearchBox {...search} />
        </header>
    );
}
