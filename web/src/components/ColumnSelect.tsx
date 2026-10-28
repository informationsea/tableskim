import { columnLabel } from '../hooks';
import type { SheetInfo } from '../protocol';

interface ColumnSelectProps {
    id: string;
    info: SheetInfo;
    value: number;
    onChange: (column: number) => void;
}

export function ColumnSelect({ id, info, value, onChange }: ColumnSelectProps) {
    return (
        <select
            id={id}
            className="control max-w-64"
            value={value}
            onChange={(event) => onChange(Number(event.target.value))}
        >
            {info.columns.map((_, column) => (
                <option key={column} value={column}>
                    {columnLabel(info, column)}
                </option>
            ))}
        </select>
    );
}
