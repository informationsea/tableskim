import { useState } from 'react';
import type { TableSkimClient } from '../client';
import { Dialog } from './Dialog';
import { useFileJob } from './fileJob';

interface ConvertDialogProps {
    client: TableSkimClient;
    fileName: string;
    onClose: () => void;
}

const FORMATS = [
    { format: 'parquet', label: 'Parquet' },
    { format: 'duckdb', label: 'DuckDB' },
] as const;

/** The GUI's Convert to Parquet / DuckDB...: the whole text file, typed. */
export function ConvertDialog({ client, fileName, onClose }: ConvertDialogProps) {
    const [format, setFormat] = useState<(typeof FORMATS)[number]['format']>('parquet');
    const { running, rows, error, saved, start, cancel } = useFileJob(client, 'convert');

    return (
        <Dialog
            id="convert-dialog"
            title="Convert to Parquet / DuckDB"
            onClose={onClose}
            footer={
                <>
                    {running ? (
                        <button type="button" className="control" onClick={() => void cancel()}>
                            Stop
                        </button>
                    ) : (
                        <button
                            type="button"
                            id="convert-start"
                            className="control"
                            onClick={() => void start(() => client.call('startConvert', { format }))}
                        >
                            Convert
                        </button>
                    )}
                    <button type="button" className="control" onClick={onClose}>
                        Close
                    </button>
                </>
            }
        >
            <p className="mb-2 text-muted">
                Every row of {fileName}, read as it is shown (its encoding, first row and ## lines), with each column
                given the narrowest type its values fit. Sorts and filters are not applied.
            </p>
            <fieldset className="mb-2 flex flex-wrap gap-3" disabled={running}>
                {FORMATS.map((item) => (
                    <label key={item.format} className="flex items-center gap-1">
                        <input
                            type="radio"
                            name="convert-format"
                            value={item.format}
                            checked={format === item.format}
                            onChange={() => setFormat(item.format)}
                        />
                        {item.label}
                    </label>
                ))}
            </fieldset>
            <p id="convert-status" className="text-muted" role="status">
                {running && `Converting… ${rows.toLocaleString()} rows`}
                {saved}
            </p>
            {error && <p className="text-danger">{error}</p>}
        </Dialog>
    );
}
