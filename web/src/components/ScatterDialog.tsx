import { useEffect, useRef, useState } from 'react';
import type { TableSkimClient } from '../client';
import type { ChartImage } from '../charts/image';
import { ScatterChart, type ScatterMode, type ScatterPick } from '../charts/ScatterChart';
import { errorText, usePolled } from '../hooks';
import { type Condition, Events, type ScatterState, type SheetInfo } from '../protocol';
import { ColumnSelect } from './ColumnSelect';
import { Dialog } from './Dialog';
import { SaveImageButtons } from './SaveImageButtons';
import type { CheckpointBannerProps } from './Banners';

interface ScatterDialogProps {
    client: TableSkimClient;
    info: SheetInfo;
    column: number; // the cursor's: X or Y is picked from it
    hidden: number[];
    onFilter: (conditions: Condition[]) => void;
    onGoToRow: (row: number) => void;
    onClose: () => void;
    checkpoint?: CheckpointBannerProps;
}

/**
 * Two columns plotted against each other over the rows of the view: points,
 * the density, or box plots for a text X.
 */
export function ScatterDialog({
    client,
    info,
    column,
    hidden,
    onFilter,
    onGoToRow,
    onClose,
    checkpoint,
}: ScatterDialogProps) {
    const [axes, setAxes] = useState<{ x: number; y: number } | null>(null);
    const [mode, setMode] = useState<ScatterMode>('points');
    const [fit, setFit] = useState(true);
    const [state, setState] = useState<ScatterState | null>(null);
    const [error, setError] = useState('');
    const sheet = info.sheet;
    const started = useRef('');
    const chart = useRef<ChartImage>(null);

    // The columns to start with, as the GUI picks them.
    useEffect(() => {
        let cancelled = false;
        client.call('defaultScatter', { sheet, column, hidden }).then(
            (picked) => !cancelled && setAxes(picked),
            (failure: unknown) => !cancelled && setError(errorText(failure)),
        );
        return () => {
            cancelled = true;
        };
        // Once, for the dialog's sheet.
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, []);

    const start = axes ? `${sheet}:${axes.x}:${axes.y}` : '';
    usePolled(
        client,
        sheet,
        Events.SCATTER,
        async () => {
            if (!axes) return;
            try {
                if (started.current !== start) {
                    started.current = start;
                    await client.call('startScatter', { sheet, x: axes.x, y: axes.y, points: true });
                }
                let next = await client.call('scatterState', { sheet });
                // The points are many: fetched once the plot is done.
                if (next.done) next = await client.call('scatterState', { sheet, points: true });
                setState(next);
                setError('');
            } catch (failure) {
                setError(errorText(failure));
            }
        },
        start,
    );

    const pick = async (picked: ScatterPick) => {
        try {
            if (picked.kind === 'row') {
                onGoToRow(picked.row);
                return;
            }
            if (!info.capabilities.filter) return;
            const where =
                picked.kind === 'cell'
                    ? { xBin: picked.xBin, yBin: picked.yBin }
                    : picked.kind === 'category'
                      ? { category: picked.category }
                      : { xLow: picked.xLow, xHigh: picked.xHigh, yLow: picked.yLow, yHigh: picked.yHigh };
            const reply = await client.call('scatterFilter', { sheet, ...where });
            // The model starts the plot over on the rows left.
            onFilter(reply.conditions);
        } catch (failure) {
            setError(errorText(failure));
        }
    };

    const shown = state && axes && state.x === axes.x && state.y === axes.y ? state : null;
    const categorical = !!shown && shown.categories.length > 0;
    const shownMode: ScatterMode = mode === 'boxes' && !categorical ? 'points' : mode;
    const progress =
        !state || state.done
            ? ''
            : `${state.counting ? 'Plotting' : 'Scanning'}… ${state.scannedRows.toLocaleString()} rows`;
    const hint =
        shownMode === 'points'
            ? 'Click a point to go to its row' + (categorical ? '.' : '; drag a rectangle to filter to it.')
            : shownMode === 'density'
              ? 'Click a cell to filter to its rows' + (categorical ? '.' : '; drag a rectangle to filter to it.')
              : 'Click a box to filter to its category.';
    return (
        <Dialog id="scatter-dialog" title="Scatter Plot" wide onClose={onClose} checkpoint={checkpoint}>
            <div className="mb-2 flex flex-wrap items-center gap-2">
                {axes && (
                    <>
                        <label className="flex items-center gap-1">
                            X
                            <ColumnSelect
                                id="scatter-x"
                                info={info}
                                value={axes.x}
                                onChange={(x) => setAxes({ ...axes, x })}
                            />
                        </label>
                        <label className="flex items-center gap-1">
                            Y
                            <ColumnSelect
                                id="scatter-y"
                                info={info}
                                value={axes.y}
                                onChange={(y) => setAxes({ ...axes, y })}
                            />
                        </label>
                        <button type="button" className="control" onClick={() => setAxes({ x: axes.y, y: axes.x })}>
                            Swap
                        </button>
                    </>
                )}
                <span className="flex" role="group" aria-label="Show">
                    {(['points', 'density', 'boxes'] as const).map((value) => (
                        <button
                            key={value}
                            type="button"
                            id={`scatter-${value}`}
                            className="control -ml-px first:ml-0"
                            aria-pressed={shownMode === value}
                            disabled={value === 'boxes' && !categorical}
                            onClick={() => setMode(value)}
                        >
                            {value === 'points' ? 'Points' : value === 'density' ? 'Density' : 'Box plots'}
                        </button>
                    ))}
                </span>
                <label className="flex items-center gap-1" title="The least-squares line over every row plotted">
                    <input
                        type="checkbox"
                        id="scatter-fit"
                        checked={fit}
                        disabled={!shown?.fit || shownMode === 'boxes'}
                        onChange={(event) => setFit(event.target.checked)}
                    />
                    {/* The line's key: its colour beside the label, not on it. */}
                    <span className="inline-block h-0.5 w-4 rounded-full bg-fit" aria-hidden="true" />
                    Regression line
                </label>
                <SaveImageButtons
                    chart={chart}
                    name={
                        axes
                            ? `${info.columns[axes.y]?.name ?? 'Y'} vs ${info.columns[axes.x]?.name ?? 'X'}`
                            : 'scatter plot'
                    }
                    enabled={!!shown?.plottable}
                    onError={setError}
                />
                <span id="scatter-progress" className="text-muted">
                    {progress}
                </span>
            </div>
            {error && <p className="mb-2 text-danger">{error}</p>}
            {shown && (
                <>
                    <p id="scatter-summary" className="mb-1 text-muted">
                        {shown.summary}
                        {shown.plottable && ` · ${hint}`}
                    </p>
                    {shown.statsText && (
                        <p id="scatter-stats" className="mb-2 text-muted">
                            {shown.statsText}
                        </p>
                    )}
                    {shown.plottable && (
                        <ScatterChart
                            ref={chart}
                            state={shown}
                            mode={shownMode}
                            fit={fit}
                            onPick={(picked) => void pick(picked)}
                        />
                    )}
                </>
            )}
        </Dialog>
    );
}
