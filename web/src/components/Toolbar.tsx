import { useRef } from 'react';
import { type BookInfo, type HeaderMode, type OpenOptions, TEXT_ENCODINGS } from '../protocol';
import { SearchBox, type SearchBoxProps } from './SearchBox';

export interface Tool {
    id: string;
    label: string;
    title: string;
    enabled: boolean;
    pressed?: boolean; // a panel's toggle
    onClick: () => void;
}

interface ToolbarProps {
    onOpen: (file: File) => void;
    // The file's book as last opened, which tells the options that apply to it.
    book: BookInfo | null;
    options: OpenOptions;
    onOptions: (options: OpenOptions) => void;
    tools: Tool[];
    search: SearchBoxProps;
}

export function Toolbar({ onOpen, book, options, onOptions, tools, search }: ToolbarProps) {
    const text = !!book?.isTextFormat;
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
            <label id="header-option" className="flex items-center gap-1" hidden={!book?.usesHeaderOption}>
                First row
                <select
                    id="header-mode"
                    className="control"
                    value={options.header ?? 'auto'}
                    onChange={(event) => onOptions({ ...options, header: event.target.value as HeaderMode })}
                >
                    <option value="auto">Auto</option>
                    <option value="yes">Header</option>
                    <option value="no">Data</option>
                </select>
            </label>
            <label id="encoding-option" className="flex items-center gap-1" hidden={!text}>
                Encoding
                <select
                    id="encoding"
                    className="control"
                    value={options.encoding ?? book?.textEncoding ?? ''}
                    onChange={(event) => onOptions({ ...options, encoding: event.target.value })}
                >
                    {TEXT_ENCODINGS.map((encoding) => (
                        <option key={encoding} value={encoding}>
                            {encoding}
                        </option>
                    ))}
                </select>
            </label>
            <label
                id="skip-meta-option"
                className="flex items-center gap-1"
                title="Leave out lines starting with ##, such as the meta-information of VCF files"
                hidden={!text}
            >
                <input
                    type="checkbox"
                    id="skip-meta-lines"
                    checked={options.skipMetaLines ?? !!book?.skipsMetaLines}
                    onChange={(event) => onOptions({ ...options, skipMetaLines: event.target.checked })}
                />
                Skip ## lines
            </label>
            {tools.map((tool) => (
                <button
                    key={tool.id}
                    type="button"
                    id={tool.id}
                    className="control"
                    title={tool.title}
                    aria-pressed={tool.pressed}
                    disabled={!tool.enabled}
                    onClick={tool.onClick}
                >
                    {tool.label}
                </button>
            ))}
            <span className="flex-1" />
            <SearchBox {...search} />
        </header>
    );
}
