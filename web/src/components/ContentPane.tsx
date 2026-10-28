import { useState } from 'react';
import type { TableSkimClient } from '../client';
import type { Cell } from '../grid/controller';
import { columnLabel, errorText, usePolled } from '../hooks';
import { Events, type SheetInfo } from '../protocol';

interface ContentPaneProps {
    client: TableSkimClient;
    info: SheetInfo;
    cell: Cell; // the top-left cell of the selection
    monospace: boolean;
}

/** The whole value of a cell, wrapped to the pane, as the GUI's Content pane shows it. */
export function ContentPane({ client, info, cell, monospace }: ContentPaneProps) {
    const [content, setContent] = useState<{ key: string; kind: string; text: string } | null>(null);
    const [error, setError] = useState('');
    const sheet = info.sheet;
    const column = Math.min(cell.column, info.columns.length - 1);
    const key = `${sheet}:${cell.row}:${column}`;

    // Again as rows load and as the view puts other rows there.
    usePolled(
        client,
        sheet,
        Events.ROWS | Events.VIEW,
        async () => {
            try {
                const reply = await client.call('cell', { sheet, row: cell.row, column });
                setContent({ key, ...reply });
                setError('');
            } catch (failure) {
                setError(errorText(failure));
            }
        },
        key,
    );

    const shown = content?.key === key ? content : null;
    return (
        <section id="content-pane" className="flex min-h-0 flex-1 flex-col">
            <h2 className="truncate border-b border-line px-2.5 py-1 text-muted" title={columnLabel(info, column)}>
                Row {(cell.row + 1).toLocaleString()} · {columnLabel(info, column)}
            </h2>
            <div className="min-h-0 flex-1 overflow-auto p-2.5">
                {error && <p className="text-danger">{error}</p>}
                {shown?.kind === 'l' && <p className="text-muted">Loading…</p>}
                {shown?.kind === 'x' && <p className="text-danger">The cell cannot be read.</p>}
                <div
                    id="content-text"
                    className={`break-words whitespace-pre-wrap select-text${monospace ? 'font-mono' : ''}`}
                >
                    {shown && shown.kind !== 'l' ? shown.text : ''}
                </div>
            </div>
        </section>
    );
}
