import { useEffect, useState } from 'react';
import type { TableSkimClient } from '../client';
import { download } from '../charts/image';
import { errorText, usePolled } from '../hooks';
import { Events, type JobState, SESSION } from '../protocol';

/**
 * Follows a job that writes a file (an export or a conversion) and hands
 * the file to the browser to save once it is written; the worker's copy is
 * then removed. Leaving the dialog stops the job.
 */
export function useFileJob(client: TableSkimClient, kind: 'export' | 'convert') {
    const [job, setJob] = useState<JobState | null>(null);
    const [running, setRunning] = useState(false);
    const [error, setError] = useState('');
    const [saved, setSaved] = useState('');

    useEffect(() => () => void client.call('cancelJob', {}).catch(() => {}), [client]);

    usePolled(
        client,
        SESSION,
        Events.JOB,
        async () => {
            if (!running) return;
            const state = await client.call('jobState', {});
            setJob(state);
            if (state.kind !== kind || !state.done) return;
            setRunning(false);
            if (state.error) {
                setError(state.error);
                return;
            }
            try {
                const data = await client.readFile(state.file ?? '');
                download(new Blob([data]), state.fileName ?? kind);
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

    const start = async (begin: () => Promise<unknown>) => {
        setError('');
        setSaved('');
        try {
            await begin();
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

    const rows = job && job.kind === kind ? job.rows : 0;
    return { running, rows, error, saved, start, cancel, setError };
}
