import { useEffect, useRef, useState } from 'react';
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
    const [starting, setStarting] = useState(false);
    const [running, setRunning] = useState(false);
    const [error, setError] = useState('');
    const [saved, setSaved] = useState('');
    const mounted = useRef(false);
    const generation = useRef(0);
    const owner = useRef<number | null>(null);

    useEffect(() => {
        mounted.current = true;
        return () => {
            mounted.current = false;
            const owned = owner.current !== null;
            owner.current = null;
            if (owned) void client.call('cancelJob', {}).catch(() => {});
        };
    }, [client]);

    const isCurrent = (run: number) => mounted.current && owner.current === run;

    const cancel = async () => {
        if (owner.current === null) return;
        // Invalidate pending replies before another run can start.
        owner.current = null;
        setStarting(false);
        setRunning(false);
        setJob(null);
        await client.call('cancelJob', {}).catch(() => {});
    };

    usePolled(
        client,
        SESSION,
        Events.JOB,
        async () => {
            const run = owner.current;
            if (!running || run === null || !isCurrent(run)) return;
            let finished = false;
            try {
                const state = await client.call('jobState', {});
                if (!isCurrent(run)) return;
                setJob(state);
                if (state.kind !== kind || !state.done) return;
                finished = true;
                if (state.error) {
                    setError(state.error);
                    return;
                }
                const data = await client.readFile(state.file ?? '');
                if (!isCurrent(run)) return;
                download(new Blob([data]), state.fileName ?? kind);
                setSaved(
                    `Saved ${state.fileName} (${(state.size ?? data.byteLength).toLocaleString()} bytes, ${state.rows.toLocaleString()} rows).`,
                );
            } catch (failure) {
                if (isCurrent(run)) {
                    setError(errorText(failure));
                    finished = true;
                }
            } finally {
                if (finished && isCurrent(run)) await cancel();
            }
        },
        running,
    );

    const start = async (begin: () => Promise<unknown>) => {
        if (!mounted.current || owner.current !== null) return;
        const run = ++generation.current;
        owner.current = run;
        setStarting(true);
        setError('');
        setSaved('');
        try {
            await begin();
            if (!isCurrent(run)) return;
            setStarting(false);
            setJob(null);
            setRunning(true);
        } catch (failure) {
            if (isCurrent(run)) {
                owner.current = null;
                setStarting(false);
                setError(errorText(failure));
            }
        }
    };

    const rows = job && job.kind === kind ? job.rows : 0;
    return { running: starting || running, rows, error, saved, start, cancel, setError };
}
