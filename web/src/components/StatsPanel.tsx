import { useState } from 'react';
import type { TableSkimClient } from '../client';
import { columnLabel, errorText, usePolled } from '../hooks';
import { Events, type SheetInfo, type StatsState } from '../protocol';

interface StatsPanelProps {
    client: TableSkimClient;
    info: SheetInfo;
    column: number; // the cursor's
}

/** Statistics of the cursor's column over the rows of the view. */
export function StatsPanel({ client, info, column }: StatsPanelProps) {
    const [state, setState] = useState<StatsState | null>(null);
    const [error, setError] = useState('');
    const sheet = info.sheet;

    // Started for each column; the model starts it over when the view changes.
    usePolled(
        client,
        sheet,
        Events.STATS,
        async () => {
            try {
                let next = await client.call('statsState', { sheet });
                if (!next.active || next.column !== column) next = await client.call('startStats', { sheet, column });
                setState(next);
                setError('');
            } catch (failure) {
                setError(errorText(failure));
            }
        },
        `${sheet}:${column}`,
    );

    const shown = state?.column === column ? state : null;
    const most = Math.max(1, ...(shown?.top.map((value) => value.count) ?? []));
    return (
        <section id="stats-panel" className="flex min-h-0 flex-1 flex-col">
            <h2 className="truncate border-b border-line px-2.5 py-1 font-semibold" title={columnLabel(info, column)}>
                {columnLabel(info, column)}
            </h2>
            <div className="min-h-0 flex-1 overflow-auto p-2.5">
                {error && <p className="text-danger">{error}</p>}
                {shown && !shown.done && (
                    <p id="stats-progress" className="mb-2 text-muted">
                        Scanning… {shown.scannedRows.toLocaleString()} rows
                    </p>
                )}
                <table id="stats-table" className="w-full">
                    <tbody>
                        {shown?.lines.map((line) => (
                            <tr key={line.label} className="border-b border-grid-line">
                                <th className="py-0.5 pr-2 text-left font-normal text-muted">{line.label}</th>
                                <td className="py-0.5 text-right tabular-nums">{line.value}</td>
                            </tr>
                        ))}
                    </tbody>
                </table>
                {shown && shown.top.length > 0 && (
                    <>
                        <h3 className="mt-3 mb-1 font-semibold">Most frequent</h3>
                        <ul id="stats-top">
                            {shown.top.map((value, i) => (
                                <li
                                    key={i}
                                    className="py-0.5"
                                    title={`${value.value}: ${value.count.toLocaleString()}`}
                                >
                                    <div className="flex gap-2">
                                        <span className="min-w-0 flex-1 truncate">{value.value || '(empty)'}</span>
                                        <span className="text-muted tabular-nums">{value.count.toLocaleString()}</span>
                                    </div>
                                    <div className="h-1 rounded-full bg-grid-line">
                                        <div
                                            className="h-1 rounded-full bg-series"
                                            style={{ width: `${(100 * value.count) / most}%` }}
                                        />
                                    </div>
                                </li>
                            ))}
                        </ul>
                    </>
                )}
            </div>
        </section>
    );
}
