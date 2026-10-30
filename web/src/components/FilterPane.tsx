import { useEffect, useRef, useState } from 'react';
import { columnLabel } from '../hooks';
import type { Condition, Filter, FilterOpInfo, SheetInfo } from '../protocol';

interface FilterPaneProps {
    info: SheetInfo;
    ops: FilterOpInfo[];
    // Applies the filter; resolves with the reason it was refused, if it was.
    onApply: (filter: Filter | null) => Promise<string | null>;
    // A condition to add for a column ("Filter by This Column…"), new each time.
    adding: { column: number; serial: number } | null;
    onAdded: () => void;
    cursorColumn: number; // "+ Add condition" starts with it
}

interface Row extends Condition {
    id: number; // a key that stays while rows above are removed
}

interface Draft {
    combine: Filter['combine'];
    rows: Row[];
}

let nextRowId = 0;

function draftOf(filter: Filter | null): Draft {
    return {
        combine: filter?.combine ?? 'and',
        rows: (filter?.conditions ?? []).map((condition) => ({ ...condition, id: ++nextRowId })),
    };
}

// The combine mode does not matter without conditions, as in the GUI.
function sameConditions(a: Filter | null, b: Filter | null) {
    const left = a?.conditions ?? [];
    const right = b?.conditions ?? [];
    if (left.length !== right.length) return false;
    if (left.length > 0 && a?.combine !== b?.combine) return false;
    return left.every(
        (condition, i) =>
            condition.column === right[i].column && condition.op === right[i].op && condition.value === right[i].value,
    );
}

/**
 * The GUI's Filter pane: Match All / Any, a column, an operator and a value
 * for each condition, edited here and applied with Apply (or Enter).
 */
export function FilterPane({ info, ops, onApply, adding, onAdded, cursorColumn }: FilterPaneProps) {
    const applied = info.view.filter;
    const [draft, setDraft] = useState(() => draftOf(applied));
    const [synced, setSynced] = useState<Filter | null>(applied);
    const [refused, setRefused] = useState<{ key: string; error: string } | null>(null);
    const [shownSheet, setShownSheet] = useState(info.sheet);
    const valueInputs = useRef(new Map<number, HTMLInputElement>());
    const focusRow = useRef<number | null>(null);
    const usable = info.capabilities.filter;
    const narrowing = applied?.narrowing ?? [];
    const lastColumn = Math.max(0, info.columns.length - 1);

    // Another sheet, or a filter set elsewhere (a cell's menu, a chart): shown
    // here. While a view is applied, the one reported is still the old one.
    if (shownSheet !== info.sheet || (!info.pending && !sameConditions(applied, synced))) {
        setShownSheet(info.sheet);
        setSynced(applied);
        setDraft(draftOf(applied));
        setRefused(null);
    }

    // The filter as edited here, with the chart selections kept.
    const edited = (from: Draft = draft, narrowed = narrowing): Filter | null => {
        const conditions = from.rows.map(({ column, op, value }) => ({
            column: Math.min(column, lastColumn),
            op,
            value: ops.find((entry) => entry.op === op)?.takesValue === false ? '' : value,
        }));
        return conditions.length > 0 || narrowed.length > 0
            ? { combine: from.combine, conditions, narrowing: narrowed }
            : null;
    };

    const apply = async (from: Draft = draft, narrowed = narrowing) => {
        if (!usable) return;
        const filter = edited(from, narrowed);
        const error = await onApply(filter);
        if (error) setRefused({ key: JSON.stringify(filter), error });
        else {
            setRefused(null);
            setSynced(filter);
        }
    };

    const addRow = (column: number) => {
        const row: Row = { id: ++nextRowId, column: Math.min(column, lastColumn), op: 'contains', value: '' };
        focusRow.current = row.id;
        setDraft((current) => ({ ...current, rows: [...current.rows, row] }));
    };

    const lastAdding = useRef<typeof adding>(null);
    useEffect(() => {
        if (adding && adding !== lastAdding.current && usable) {
            lastAdding.current = adding;
            addRow(adding.column);
            // Consume the request so remounting the pane does not replay it.
            onAdded();
        }
        // addRow only reads the sheet's column count.
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [adding, usable, onAdded]);

    useEffect(() => {
        if (focusRow.current === null) return;
        // The row may not be drawn yet: then on a later render.
        const input = valueInputs.current.get(focusRow.current);
        if (!input) return;
        focusRow.current = null;
        input.focus();
        input.scrollIntoView({ block: 'nearest' });
    });

    const change = (id: number, part: Partial<Condition>) =>
        setDraft((current) => ({
            ...current,
            rows: current.rows.map((row) => (row.id === id ? { ...row, ...part } : row)),
        }));

    const current = edited();
    let status: string;
    if (info.state === 'error') status = '';
    else if (!usable) status = 'Still opening (cannot filter yet).';
    else if (refused?.key === JSON.stringify(current)) status = refused.error;
    else if (info.pending) status = 'Filtering…';
    else if (!sameConditions(current, synced)) status = 'Press Apply (or Enter) to filter.';
    else status = applied ? info.rowsText : `No filter (${info.rowsText})`;

    return (
        <section id="filter-pane" className="flex min-h-0 flex-1 flex-col">
            <div className="flex items-center gap-3 px-2.5 pt-2" role="radiogroup" aria-label="Match">
                <span>Match:</span>
                {(['and', 'or'] as const).map((combine) => (
                    <label
                        key={combine}
                        className="flex items-center gap-1"
                        title={
                            combine === 'and'
                                ? 'Show rows that match all conditions (AND)'
                                : 'Show rows that match any condition (OR)'
                        }
                    >
                        <input
                            type="radio"
                            name="filter-combine"
                            id={`filter-match-${combine === 'and' ? 'all' : 'any'}`}
                            value={combine}
                            checked={draft.combine === combine}
                            disabled={!usable}
                            onChange={() => setDraft((current) => ({ ...current, combine }))}
                        />
                        {combine === 'and' ? 'All' : 'Any'}
                    </label>
                ))}
            </div>
            {narrowing.length > 0 && (
                <button
                    type="button"
                    id="filter-narrowing"
                    className="control mx-2.5 mt-2 text-left"
                    disabled={!usable}
                    title={`Chart selections are ANDed with the conditions below. Click to remove the additional narrowing.\n${narrowing
                        .map(
                            (c) =>
                                `${info.columns[c.column]?.name ?? c.column} ${ops.find((o) => o.op === c.op)?.label ?? c.op} ${c.value}`,
                        )
                        .join(' AND ')}`}
                    onClick={() => void apply(draft, [])}
                >
                    Additional narrowing ({narrowing.length}) - Clear
                </button>
            )}
            <form
                id="filter-form"
                className="min-h-0 flex-1 overflow-auto px-2.5 py-2"
                autoComplete="off"
                onSubmit={(event) => {
                    event.preventDefault();
                    void apply();
                }}
            >
                <ul id="filter-conditions" className="flex flex-col gap-3">
                    {draft.rows.map((row) => {
                        const takesValue = ops.find((entry) => entry.op === row.op)?.takesValue ?? true;
                        return (
                            <li key={row.id} className="flex flex-col gap-1">
                                <div className="flex gap-1">
                                    <select
                                        className="filter-column control min-w-0 flex-1"
                                        aria-label="Column"
                                        value={Math.min(row.column, lastColumn)}
                                        disabled={!usable}
                                        onChange={(event) => change(row.id, { column: Number(event.target.value) })}
                                    >
                                        {info.columns.map((_, column) => (
                                            <option key={column} value={column}>
                                                {columnLabel(info, column)}
                                            </option>
                                        ))}
                                    </select>
                                    <button
                                        type="button"
                                        className="filter-remove control px-2"
                                        title="Remove this condition"
                                        aria-label="Remove this condition"
                                        disabled={!usable}
                                        onClick={() => {
                                            const next = {
                                                ...draft,
                                                rows: draft.rows.filter((other) => other.id !== row.id),
                                            };
                                            setDraft(next);
                                            void apply(next);
                                        }}
                                    >
                                        ×
                                    </button>
                                </div>
                                <div className="flex gap-1">
                                    <select
                                        className="filter-op control w-28 shrink-0"
                                        aria-label="Operator"
                                        value={row.op}
                                        disabled={!usable}
                                        onChange={(event) => change(row.id, { op: event.target.value })}
                                    >
                                        {ops.map((entry) => (
                                            <option key={entry.op} value={entry.op}>
                                                {entry.label}
                                            </option>
                                        ))}
                                    </select>
                                    <input
                                        ref={(input) => {
                                            if (input) valueInputs.current.set(row.id, input);
                                            else valueInputs.current.delete(row.id);
                                        }}
                                        className="filter-value control min-w-0 flex-1"
                                        aria-label="Value"
                                        disabled={!usable || !takesValue}
                                        value={takesValue ? row.value : ''}
                                        onChange={(event) => change(row.id, { value: event.target.value })}
                                    />
                                </div>
                            </li>
                        );
                    })}
                </ul>
                <button
                    type="button"
                    id="filter-add"
                    className="control mt-3"
                    disabled={!usable || info.columns.length === 0}
                    onClick={() => addRow(cursorColumn)}
                >
                    + Add condition
                </button>
                {/* Enter in a value field applies, as in the GUI. */}
                <button type="submit" hidden tabIndex={-1} />
            </form>
            <div className="border-t border-line px-2.5 py-2">
                <div className="flex gap-1">
                    <button
                        type="button"
                        id="filter-apply"
                        className="control"
                        disabled={!usable}
                        onClick={() => void apply()}
                    >
                        Apply
                    </button>
                    <button
                        type="button"
                        id="filter-clear"
                        className="control"
                        title="Remove all conditions"
                        disabled={!usable}
                        onClick={() => {
                            const next = { ...draft, rows: [] };
                            setDraft(next);
                            void apply(next, []);
                        }}
                    >
                        Clear
                    </button>
                </div>
                <p id="filter-status" className="mt-1 break-words text-muted" title={status}>
                    {status}
                </p>
            </div>
        </section>
    );
}
