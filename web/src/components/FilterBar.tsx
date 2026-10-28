import { useState } from 'react';
import type { Column, Condition, Filter, FilterOpInfo } from '../protocol';

interface FilterBarProps {
    columns: Column[];
    filter: Filter | null;
    ops: FilterOpInfo[];
    onChange: (filter: Filter | null) => void;
}

export function FilterBar({ columns, filter, ops, onChange }: FilterBarProps) {
    const [column, setColumn] = useState(0);
    const [op, setOp] = useState(ops[0]?.op ?? 'contains');
    const [value, setValue] = useState('');
    const selectedColumn = Math.min(column, Math.max(0, columns.length - 1));
    const conditions = filter?.conditions ?? [];
    const combine = filter?.combine ?? 'and';
    const takesValue = ops.find((entry) => entry.op === op)?.takesValue ?? true;
    const update = (next: Condition[], nextCombine = combine) =>
        onChange(next.length > 0 ? { combine: nextCombine, conditions: next } : null);

    return (
        <section
            id="filter-bar"
            className="flex flex-wrap items-center gap-2 border-b border-line bg-chrome px-2.5 py-1.5"
        >
            <select
                id="filter-combine"
                className="control"
                aria-label="Combine conditions"
                value={combine}
                onChange={(event) => update(conditions, event.target.value as Filter['combine'])}
            >
                <option value="and">All of</option>
                <option value="or">Any of</option>
            </select>
            <ul id="filter-conditions" className="contents">
                {conditions.map((condition, index) => {
                    const info = ops.find((entry) => entry.op === condition.op);
                    return (
                        <li
                            key={index}
                            className="flex items-center gap-1 rounded-full border border-line bg-surface py-0.5 pr-1 pl-2"
                        >
                            <span>
                                {columns[condition.column]?.name ?? condition.column} {info?.label ?? condition.op}
                                {info?.takesValue ? ` "${condition.value}"` : ''}
                            </span>
                            <button
                                type="button"
                                className="cursor-pointer px-1"
                                title="Remove this condition"
                                onClick={() => update(conditions.filter((_, i) => i !== index))}
                            >
                                ×
                            </button>
                        </li>
                    );
                })}
            </ul>
            <form
                id="filter-form"
                className="flex min-w-0 flex-wrap gap-1"
                autoComplete="off"
                onSubmit={(event) => {
                    event.preventDefault();
                    update([...conditions, { column: selectedColumn, op, value }]);
                }}
            >
                <select
                    id="filter-column"
                    className="control max-w-48"
                    aria-label="Column"
                    value={selectedColumn}
                    onChange={(event) => setColumn(Number(event.target.value))}
                >
                    {columns.map((entry, index) => (
                        <option key={index} value={index}>
                            {entry.name}
                        </option>
                    ))}
                </select>
                <select
                    id="filter-op"
                    className="control max-w-48"
                    aria-label="Operator"
                    value={op}
                    onChange={(event) => setOp(event.target.value)}
                >
                    {ops.map((entry) => (
                        <option key={entry.op} value={entry.op}>
                            {entry.label}
                        </option>
                    ))}
                </select>
                <input
                    id="filter-value"
                    className="control min-w-0 flex-[1_1_8em]"
                    placeholder="Value"
                    aria-label="Value"
                    disabled={!takesValue}
                    value={value}
                    onChange={(event) => setValue(event.target.value)}
                />
                <button type="submit" className="control">
                    Add
                </button>
            </form>
            <button type="button" id="filter-clear" className="control" onClick={() => onChange(null)}>
                Clear
            </button>
        </section>
    );
}
