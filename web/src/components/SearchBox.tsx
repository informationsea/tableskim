import type { Ref } from 'react';

export interface SearchBoxProps
{
    text: string;
    caseSensitive: boolean;
    regex: boolean;
    enabled: boolean;
    status: string;
    onChange: (change: { text?: string; caseSensitive?: boolean; regex?: boolean }) => void;
    // Starts the search, or moves to the next or previous hit.
    onFind: (forward: boolean) => void;
    onLeave: () => void;
    inputRef?: Ref<HTMLInputElement>;
}

export function SearchBox({ text, caseSensitive, regex, enabled, status, onChange, onFind, onLeave, inputRef }: SearchBoxProps)
{
    return (
        <form
            id="search-form"
            className="flex flex-wrap items-center gap-1"
            autoComplete="off"
            onSubmit={(event) => event.preventDefault()}
        >
            <input
                type="search"
                id="search-text"
                ref={inputRef}
                className="control w-56 max-w-[40vw] max-sm:w-36"
                placeholder="Search"
                aria-label="Search"
                disabled={!enabled}
                value={text}
                onChange={(event) => onChange({ text: event.target.value })}
                onKeyDown={(event) =>
                {
                    if (event.key === 'Enter')
                    {
                        event.preventDefault();
                        onFind(!event.shiftKey);
                    }
                    else if (event.key === 'Escape')
                        onLeave();
                }}
            />
            <label className="flex items-center gap-0.5 font-mono" title="Match case">
                <input
                    type="checkbox"
                    id="search-case"
                    checked={caseSensitive}
                    onChange={(event) => onChange({ caseSensitive: event.target.checked })}
                />
                Aa
            </label>
            <label className="flex items-center gap-0.5 font-mono" title="Regular expression">
                <input
                    type="checkbox"
                    id="search-regex"
                    checked={regex}
                    onChange={(event) => onChange({ regex: event.target.checked })}
                />
                .*
            </label>
            <button type="button" id="search-prev" className="control" title="Previous hit (Shift+Enter)" disabled={!enabled}
                    onClick={() => onFind(false)}>
                ↑
            </button>
            <button type="button" id="search-next" className="control" title="Next hit (Enter)" disabled={!enabled}
                    onClick={() => onFind(true)}>
                ↓
            </button>
            <span id="search-status" className="text-muted">{status}</span>
        </form>
    );
}
