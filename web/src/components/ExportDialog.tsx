import { useEffect, useState } from 'react';
import type { TableSkimClient } from '../client';
import { errorText } from '../hooks';
import type { ExportFormat, ExportFormatInfo, SheetInfo } from '../protocol';
import { Dialog } from './Dialog';
import { useFileJob } from './fileJob';
import type { CheckpointBannerProps } from './Banners';

interface ExportDialogProps {
    client: TableSkimClient;
    info: SheetInfo;
    hidden: number[];
    onClose: () => void;
    checkpoint?: CheckpointBannerProps;
}

/** Saves the sheet as shown (its sort and filter, without hidden columns). */
export function ExportDialog({ client, info, hidden, onClose, checkpoint }: ExportDialogProps) {
    const [formats, setFormats] = useState<ExportFormatInfo[]>([]);
    const [format, setFormat] = useState<ExportFormat>('csv');
    const { running, rows, error, saved, start, cancel, setError } = useFileJob(client, 'export');

    useEffect(() => {
        client.call('exportFormats', {}).then(setFormats, (failure: unknown) => setError(errorText(failure)));
        // setError is a state setter.
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [client]);

    const startExport = () => start(() => client.call('startExport', { sheet: info.sheet, format, hidden }));

    return (
        <Dialog
            id="export-dialog"
            title="Export View"
            onClose={onClose}
            checkpoint={checkpoint}
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
                {running && `Writing… ${rows.toLocaleString()} rows`}
                {saved}
            </p>
            {error && <p className="text-danger">{error}</p>}
        </Dialog>
    );
}
