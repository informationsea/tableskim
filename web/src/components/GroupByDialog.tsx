import { useEffect, useState } from 'react';
import type { TableSkimClient } from '../client';
import { columnLabel, errorText, usePolled } from '../hooks';
import { Events, type JobState, SESSION, type SheetInfo } from '../protocol';
import { Dialog } from './Dialog';

interface GroupByDialogProps {
    client: TableSkimClient;
    info: SheetInfo;
    column: number; // the cursor's: grouped by at first
    hidden: number[];
    onDone: (sheet: string, sheets: string[]) => void;
    onClose: () => void;
}

const DEFAULT_AGGREGATES = ['count', 'mean'];

/** Groups the rows of the view by key columns into a result sheet. */
export function GroupByDialog({ client, info, column, hidden, onDone, onClose }: GroupByDialogProps) {
    const [aggregates, setAggregates] = useState<string[]>([]);
    const [keys, setKeys] = useState<number[]>([column]);
    const [values, setValues] = useState<number[]>([]);
    const [chosen, setChosen] = useState<string[]>(DEFAULT_AGGREGATES);
    const [running, setRunning] = useState(false);
    const [job, setJob] = useState<JobState | null>(null);
    const [error, setError] = useState('');

    useEffect(() => {
        client.call('aggregates', {}).then(setAggregates, (failure: unknown) => setError(errorText(failure)));
    }, [client]);

    usePolled(
        client,
        SESSION,
        Events.JOB,
        async () => {
            if (!running) return;
            const state = await client.call('jobState', {});
            setJob(state);
            if (state.kind !== 'groupBy' || !state.done) return;
            setRunning(false);
            await client.call('cancelJob', {});
            if (state.error) setError(state.error);
            else if (state.sheet && state.sheets) onDone(state.sheet, state.sheets);
        },
        running,
    );

    const toggle = (list: number[], item: number) =>
        list.includes(item) ? list.filter((x) => x !== item) : [...list, item].sort((a, b) => a - b);

    const run = async () => {
        setError('');
        try {
            await client.call('startGroupBy', { sheet: info.sheet, keys, values, aggregates: chosen });
            setJob(null);
            setRunning(true);
        } catch (failure) {
            setError(errorText(failure));
        }
    };

    const shown = info.columns.map((_, i) => i).filter((i) => !hidden.includes(i));
    return (
        <Dialog
            id="group-by-dialog"
            title="Group By"
            onClose={onClose}
            footer={
                <>
                    <button
                        type="button"
                        id="group-by-run"
                        className="control"
                        disabled={running || (keys.length === 0 && values.length === 0)}
                        onClick={() => void run()}
                    >
                        Group
                    </button>
                    <button type="button" className="control" onClick={onClose}>
                        Close
                    </button>
                </>
            }
        >
            <p className="mb-2 text-muted">
                Groups the rows of the view by the key columns; the result opens as a sheet.
            </p>
            <div className="mb-3 max-h-64 overflow-auto rounded border border-line">
                <table className="w-full">
                    <thead className="sticky top-0 bg-chrome">
                        <tr className="text-left">
                            <th className="px-2 py-1 font-semibold">Column</th>
                            <th className="px-2 py-1 text-center font-semibold">Key</th>
                            <th className="px-2 py-1 text-center font-semibold">Aggregate</th>
                        </tr>
                    </thead>
                    <tbody>
                        {shown.map((i) => (
                            <tr key={i} className="border-t border-grid-line">
                                <td className="px-2 py-0.5">{columnLabel(info, i)}</td>
                                <td className="px-2 py-0.5 text-center">
                                    <input
                                        type="checkbox"
                                        className="group-key"
                                        aria-label={`Group by ${info.columns[i].name}`}
                                        checked={keys.includes(i)}
                                        onChange={() => setKeys(toggle(keys, i))}
                                    />
                                </td>
                                <td className="px-2 py-0.5 text-center">
                                    <input
                                        type="checkbox"
                                        className="group-value"
                                        aria-label={`Aggregate ${info.columns[i].name}`}
                                        checked={values.includes(i)}
                                        onChange={() => setValues(toggle(values, i))}
                                    />
                                </td>
                            </tr>
                        ))}
                    </tbody>
                </table>
            </div>
            <fieldset className="mb-2 flex flex-wrap gap-x-3 gap-y-1">
                <legend className="mb-1 font-semibold">Aggregates of each column</legend>
                {aggregates.map((name) => (
                    <label key={name} className="flex items-center gap-1">
                        <input
                            type="checkbox"
                            className="group-aggregate"
                            value={name}
                            checked={chosen.includes(name)}
                            onChange={() =>
                                setChosen(chosen.includes(name) ? chosen.filter((x) => x !== name) : [...chosen, name])
                            }
                        />
                        {name}
                    </label>
                ))}
            </fieldset>
            <p className="text-muted" role="status">
                {running && `Grouping… ${(job && job.kind === 'groupBy' ? job.rows : 0).toLocaleString()} rows`}
            </p>
            {error && <p className="text-danger">{error}</p>}
        </Dialog>
    );
}
