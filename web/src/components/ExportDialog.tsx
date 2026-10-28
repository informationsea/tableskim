import { useEffect, useState } from 'react';
import type { TableSkimClient } from '../client';
import { errorText, usePolled } from '../hooks';
import { Events, type ExportFormat, type ExportFormatInfo, type JobState, SESSION, type SheetInfo } from '../protocol';
import { Dialog } from './Dialog';

interface ExportDialogProps {
    client: TableSkimClient;
    info: SheetInfo;
    hidden: number[];
    onClose: () => void;
}

/** Saves the sheet as shown (its sort and filter, without hidden columns). */
export function ExportDialog({ client, info, hidden, onClose }: ExportDialogProps) {
    const [formats, setFormats] = useState<ExportFormatInfo[]>([]);
    const [format, setFormat] = useState<ExportFormat>('csv');
    const [job, setJob] = useState<JobState | null>(null);
    const [running, setRunning] = useState(false);
    const [error, setError] = useState('');
    const [saved, setSaved] = useState('');

    useEffect(() => {
        client.call('exportFormats', {}).then(setFormats, (failure: unknown) => setError(errorText(failure)));
    }, [client]);

    // Leaving the dialog stops an export and frees its file.
    useEffect(() => () => void client.call('cancelJob', {}).catch(() => {}), [client]);

    usePolled(
        client,
        SESSION,
        Events.JOB,
        async () => {
            if (!running) return;
            const state = await client.call('jobState', {});
            setJob(state);
            if (state.kind !== 'export' || !state.done) return;
            setRunning(false);
            if (state.error) {
                setError(state.error);
                return;
            }
            try {
                // Saved by the browser, then removed from the worker's memory.
                const data = await client.readFile(state.file ?? '');
                const url = URL.createObjectURL(new Blob([data]));
                const link = document.createElement('a');
                link.href = url;
                link.download = state.fileName ?? 'export';
                link.click();
                setTimeout(() => URL.revokeObjectURL(url), 60000);
                setSaved(
                    `Saved ${state.fileName} (${(state.size ?? data.byteLength).toLocaleString()} bytes, ${state.rows.toLocaleString()} rows).`,
                );
            } catch (failure) {
                setError(errorText(failure));
            } finally {
                await client.call('cancelJob', {});
            }
        },
        running,
    );

    const startExport = async () => {
        setError('');
        setSaved('');
        try {
            await client.call('startExport', { sheet: info.sheet, format, hidden });
            setJob(null);
            setRunning(true);
        } catch (failure) {
            setError(errorText(failure));
        }
    };

    const cancel = async () => {
        await client.call('cancelJob', {}).catch(() => {});
        setRunning(false);
        setJob(null);
    };

    return (
        <Dialog
            id="export-dialog"
            title="Export View"
            onClose={onClose}
            footer={
                <>
                    {running ? (
                        <button type="button" className="control" onClick={() => void cancel()}>
                            Stop
                        </button>
                    ) : (
                        <button type="button" id="export-start" className="control" onClick={() => void startExport()}>
                            Export
                        </button>
                    )}
                    <button type="button" className="control" onClick={onClose}>
                        Close
                    </button>
                </>
            }
        >
            <p className="mb-2 text-muted">
                The rows as shown: the sort and filter applied, without the hidden columns
                {info.filterText ? ` (filter: ${info.filterText})` : ''}.
            </p>
            <fieldset className="mb-2 flex flex-wrap gap-3" disabled={running}>
                {formats.map((item) => (
                    <label key={item.format} className="flex items-center gap-1">
                        <input
                            type="radio"
                            name="export-format"
                            value={item.format}
                            checked={format === item.format}
                            onChange={() => setFormat(item.format)}
                        />
                        {item.label}
                    </label>
                ))}
            </fieldset>
            <p id="export-status" className="text-muted" role="status">
                {running && `Writing… ${(job && job.kind === 'export' ? job.rows : 0).toLocaleString()} rows`}
                {saved}
            </p>
            {error && <p className="text-danger">{error}</p>}
        </Dialog>
    );
}
