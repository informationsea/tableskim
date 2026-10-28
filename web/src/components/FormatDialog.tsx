import { useEffect, useState } from 'react';
import type { TableSkimClient } from '../client';
import { columnLabel, errorText } from '../hooks';
import type { ColumnFormat, DateStyleInfo, SheetInfo } from '../protocol';
import { Dialog } from './Dialog';

interface FormatDialogProps {
    client: TableSkimClient;
    info: SheetInfo;
    column: number;
    onApplied: () => void;
    onClose: () => void;
}

const PLAIN: ColumnFormat = { thousands: false, decimals: null, date: 'iso' };

/** How a column shows its values; copies and exports keep the values. */
export function FormatDialog({ client, info, column, onApplied, onClose }: FormatDialogProps) {
    const [format, setFormat] = useState<ColumnFormat>(info.columns[column]?.format ?? PLAIN);
    const [styles, setStyles] = useState<DateStyleInfo[]>([]);
    const [error, setError] = useState('');

    useEffect(() => {
        client.call('dateStyles', {}).then(setStyles, (failure: unknown) => setError(errorText(failure)));
    }, [client]);

    const apply = async (applied: ColumnFormat) => {
        try {
            await client.call('setFormat', { sheet: info.sheet, column, ...applied });
            onApplied();
            onClose();
        } catch (failure) {
            setError(errorText(failure));
        }
    };

    return (
        <Dialog
            id="format-dialog"
            title={`Format of ${columnLabel(info, column)}`}
            onClose={onClose}
            footer={
                <>
                    <button type="button" className="control" onClick={() => void apply(PLAIN)}>
                        Reset
                    </button>
                    <button type="button" id="format-apply" className="control" onClick={() => void apply(format)}>
                        Apply
                    </button>
                    <button type="button" className="control" onClick={onClose}>
                        Cancel
                    </button>
                </>
            }
        >
            <div className="grid grid-cols-[auto_1fr] items-center gap-x-3 gap-y-2">
                <span>Numbers</span>
                <label className="flex items-center gap-1">
                    <input
                        type="checkbox"
                        id="format-thousands"
                        checked={format.thousands}
                        onChange={(event) => setFormat({ ...format, thousands: event.target.checked })}
                    />
                    Thousands separators
                </label>
                <label htmlFor="format-decimals">Decimal places</label>
                <select
                    id="format-decimals"
                    className="control w-32"
                    value={format.decimals ?? ''}
                    onChange={(event) =>
                        setFormat({
                            ...format,
                            decimals: event.target.value === '' ? null : Number(event.target.value),
                        })
                    }
                >
                    <option value="">As stored</option>
                    {Array.from({ length: 11 }, (_, i) => (
                        <option key={i} value={i}>
                            {i}
                        </option>
                    ))}
                </select>
                <label htmlFor="format-date">Dates</label>
                <select
                    id="format-date"
                    className="control w-48"
                    value={format.date}
                    onChange={(event) => setFormat({ ...format, date: event.target.value as ColumnFormat['date'] })}
                >
                    {styles.map((style) => (
                        <option key={style.style} value={style.style}>
                            {style.example}
                        </option>
                    ))}
                </select>
            </div>
            <p className="mt-3 text-muted">Only the display changes: copies and exports keep the values.</p>
            {error && <p className="mt-2 text-danger">{error}</p>}
        </Dialog>
    );
}
