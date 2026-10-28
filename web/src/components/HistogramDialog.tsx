import { useRef, useState } from 'react';
import type { TableSkimClient } from '../client';
import { HistogramChart } from '../charts/HistogramChart';
import { errorText, usePolled } from '../hooks';
import { type Condition, Events, type HistogramState, type SheetInfo } from '../protocol';
import { ColumnSelect } from './ColumnSelect';
import { Dialog } from './Dialog';
import type { CheckpointBannerProps } from './Banners';

interface HistogramDialogProps {
    client: TableSkimClient;
    info: SheetInfo;
    column: number;
    onFilter: (conditions: Condition[]) => void;
    onClose: () => void;
    checkpoint?: CheckpointBannerProps;
}

const DEFAULT_BINS = 30;

/** A column's histogram over the rows of the view; a bar's click filters to it. */
export function HistogramDialog({
    client,
    info,
    column: initialColumn,
    onFilter,
    onClose,
    checkpoint,
}: HistogramDialogProps) {
    const [column, setColumn] = useState(initialColumn);
    const [bins, setBins] = useState(DEFAULT_BINS);
    const [state, setState] = useState<HistogramState | null>(null);
    const [error, setError] = useState('');
    const [table, setTable] = useState(false);
    const sheet = info.sheet;

    const start = `${sheet}:${column}:${bins}`;
    const started = useRef(''); // what the last startHistogram was for
    usePolled(
        client,
        sheet,
        Events.HISTOGRAM,
        async () => {
            try {
                if (started.current !== start) {
                    started.current = start;
                    setState(await client.call('startHistogram', { sheet, column, bins }));
                } else setState(await client.call('histogramState', { sheet }));
                setError('');
            } catch (failure) {
                setError(errorText(failure));
            }
        },
        start,
    );

    const pick = async (bin: number) => {
        if (!info.capabilities.filter) return;
        try {
            const reply = await client.call('histogramFilter', { sheet, bin });
            // The model starts the histogram over on the rows left.
            onFilter(reply.conditions);
        } catch (failure) {
            setError(errorText(failure));
        }
    };

    const histogram = state?.column === column ? state.histogram : null;
    const progress =
        !state || state.done
            ? ''
            : `${state.counting ? 'Counting' : 'Scanning'}… ${state.scannedRows.toLocaleString()} rows`;
    const items = histogram
        ? histogram.bins.length > 0
            ? histogram.bins
            : histogram.top.map((top) => ({ label: top.value, range: top.value, count: top.count }))
        : [];
    return (
        <Dialog id="histogram-dialog" title="Histogram" wide onClose={onClose} checkpoint={checkpoint}>
            <div className="mb-2 flex flex-wrap items-center gap-2">
                <label className="flex items-center gap-1">
                    Column
                    <ColumnSelect id="histogram-column" info={info} value={column} onChange={setColumn} />
                </label>
                <label className="flex items-center gap-1">
                    Bins
                    <input
                        id="histogram-bins"
                        type="number"
                        className="control w-20"
                        min={2}
                        max={200}
                        value={bins}
                        onChange={(event) => {
                            const value = Number(event.target.value);
                            if (value >= 2 && value <= 200) setBins(value);
                        }}
                    />
                </label>
                <button type="button" className="control" aria-pressed={table} onClick={() => setTable(!table)}>
                    Table
                </button>
                <span id="histogram-progress" className="text-muted">
                    {progress}
                </span>
            </div>
            {error && <p className="mb-2 text-danger">{error}</p>}
            {histogram && (
                <>
                    <p id="histogram-summary" className="mb-2 text-muted">
                        {histogram.summary}
                        {info.capabilities.filter && items.length > 0 && ' · Click a bar to filter to its rows.'}
                    </p>
                    {table ? (
                        <table id="histogram-table" className="w-full">
                            <thead>
                                <tr className="border-b border-line text-left">
                                    <th className="py-0.5 font-semibold">
                                        {histogram.bins.length > 0 ? 'Range' : 'Value'}
                                    </th>
                                    <th className="py-0.5 text-right font-semibold">Rows</th>
                                </tr>
                            </thead>
                            <tbody>
                                {items.map((item, i) => (
                                    <tr key={i} className="border-b border-grid-line">
                                        <td className="py-0.5">{item.range || '(empty)'}</td>
                                        <td className="py-0.5 text-right tabular-nums">
                                            {item.count.toLocaleString()}
                                        </td>
                                    </tr>
                                ))}
                            </tbody>
                        </table>
                    ) : (
                        <HistogramChart histogram={histogram} rows={info.rows} onPick={(bin) => void pick(bin)} />
                    )}
                </>
            )}
        </Dialog>
    );
}
