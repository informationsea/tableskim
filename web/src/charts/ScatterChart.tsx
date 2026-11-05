import { type Ref, useEffect, useImperativeHandle, useLayoutEffect, useMemo, useRef, useState } from 'react';
import type { ScatterAxis, ScatterState } from '../protocol';
import { groupColorName, LEGEND_LINE, legendLayout, SWATCH, type ValueGroups } from './groups';
import { type ChartImage, SvgContext, surfaceColor } from './image';
import { compactCount, observeWidth } from './scale';
import { Tooltip } from './Tooltip';

export type ScatterMode = 'points' | 'density' | 'boxes';

export type ScatterPick =
    | { kind: 'row'; row: number }
    | { kind: 'cell'; xBin: number; yBin: number }
    | { kind: 'category'; category: number }
    | { kind: 'color'; color: number } // an entry of the legend
    | { kind: 'range'; xLow: number; xHigh: number; yLow: number; yHigh: number };

interface ScatterChartProps {
    state: ScatterState;
    mode: ScatterMode;
    fit: boolean; // draw the regression line
    onPick: (pick: ScatterPick) => void;
    ref?: Ref<ChartImage>;
}

// What the plot draws with: a canvas, or an SvgContext for a saved image.
type Painter = Pick<
    CanvasRenderingContext2D,
    | 'fillStyle'
    | 'strokeStyle'
    | 'lineWidth'
    | 'lineCap'
    | 'globalAlpha'
    | 'font'
    | 'textAlign'
    | 'textBaseline'
    | 'save'
    | 'restore'
    | 'beginPath'
    | 'moveTo'
    | 'lineTo'
    | 'rect'
    | 'arc'
    | 'clip'
    | 'fill'
    | 'stroke'
    | 'fillRect'
    | 'strokeRect'
    | 'fillText'
>;

const HEIGHT = 420;
const MARGIN = { top: 12, right: 16, bottom: 44, left: 64 };
const LEGEND_WIDTH = 64;
const STEPS = 5; // --seq-1 .. --seq-5
// Points: few are drawn large with a ring of the surface colour, so that
// they stay apart where they overlap; many are drawn small and translucent.
const FEW_POINTS = 1000;
const POINT_RADIUS = 4;
const SMALL_POINT_RADIUS = 2.5;
// Between the frame and the plotted range, so that marks at the ends show whole.
const PAD = 8;
const HIT_RADIUS = 12;
const DRAG_THRESHOLD = 4;
// Of a category's column: where its points spread, its box, its density cells.
const SPREAD_WIDTH = 0.7;
const BOX_WIDTH = 0.6;
const CELL_WIDTH = 0.9;

// The coordinates an axis spans: its ticks, and the density bins too.
function axisRange(axis: ScatterAxis): [number, number] {
    const values = [...axis.ticks.map((tick) => tick.at), ...axis.edges];
    let low = values.length ? Math.min(...values) : 0;
    let high = values.length ? Math.max(...values) : 1;
    if (!(high > low)) high = low + 1;
    if (!Number.isFinite(low)) [low, high] = [0, 1];
    return [low, high];
}

// Where in its category's column a row's point is drawn, from -0.5 to
// 0.5: the hash the GUI uses, so that both draw the same plot.
function spreadOf(row: number): number {
    const mask = (1n << 64n) - 1n;
    let h = (BigInt(row) * 0x9e3779b97f4a7c15n) & mask;
    h ^= h >> 29n;
    h = (h * 0xbf58476d1ce4e5b9n) & mask;
    h ^= h >> 32n;
    return Number(h % 10000n) / 10000 - 0.5;
}

function shade(count: number, most: number): number {
    // Log steps: a few rows stay visible beside crowded cells.
    return Math.min(STEPS, Math.max(1, Math.ceil((STEPS * Math.log1p(count)) / Math.log1p(Math.max(1, most)))));
}

const numberText = (value: number) => value.toLocaleString(undefined, { maximumSignificantDigits: 6 });

export function ScatterChart({ state, mode, fit, onPick, ref }: ScatterChartProps) {
    const box = useRef<HTMLDivElement>(null);
    const canvas = useRef<HTMLCanvasElement>(null);
    const [width, setWidth] = useState(600);
    const [hover, setHover] = useState<{ x: number; y: number; value: string; label: string } | null>(null);
    const [hoveredColor, setHoveredColor] = useState<number | null>(null); // an entry of the legend
    const [band, setBand] = useState<{ x0: number; y0: number; x1: number; y1: number } | null>(null);
    const press = useRef<{ x: number; y: number } | null>(null);
    useEffect(() => (box.current ? observeWidth(box.current, setWidth) : undefined), []);

    const categorical = state.categories.length > 0;
    const xBins = categorical ? state.categories.length : Math.max(0, state.xAxis.edges.length - 1);
    const yBins = Math.max(0, state.yAxis.edges.length - 1);
    const legend = mode === 'density';
    // Coloured points: a legend of the colours with rows, above the plot.
    const colored = mode === 'points' && state.colors.length > 0;
    const groups: ValueGroups = { names: state.colors, other: state.otherColor, empty: state.emptyColor };
    const plotWidth = Math.max(40, width - MARGIN.left - MARGIN.right - (legend ? LEGEND_WIDTH : 0));
    const colorLegend = legendLayout(
        colored
            ? state.colors
                  .map((name, group) => ({
                      group,
                      label: `${name} (${(state.colorPairs[group] ?? 0).toLocaleString()})`,
                  }))
                  .filter(({ group }) => (state.colorPairs[group] ?? 0) > 0)
            : [],
        plotWidth,
    );
    const height = HEIGHT + colorLegend.height;
    const plot = {
        x: MARGIN.left,
        y: MARGIN.top + colorLegend.height,
        w: plotWidth,
        h: HEIGHT - MARGIN.top - MARGIN.bottom,
    };
    const [xLow, xHigh] = categorical ? [-0.5, state.categories.length - 0.5] : axisRange(state.xAxis);
    const [yLow, yHigh] = axisRange(state.yAxis);
    const inner = { x: plot.x + PAD, y: plot.y + PAD, w: plot.w - 2 * PAD, h: plot.h - 2 * PAD };
    const toX = (value: number) => inner.x + ((value - xLow) / (xHigh - xLow)) * inner.w;
    const toY = (value: number) => inner.y + inner.h - ((value - yLow) / (yHigh - yLow)) * inner.h;
    const fromX = (px: number) => xLow + ((px - inner.x) / inner.w) * (xHigh - xLow);
    const fromY = (py: number) => yLow + ((inner.y + inner.h - py) / inner.h) * (yHigh - yLow);

    // Points as screen coordinates with their rows and colours.
    const points = useMemo(() => {
        const result: { x: number; y: number; row: number; color: number }[] = [];
        for (let i = 0; i + 2 < state.points.length; i += 3) {
            const row = state.points[i + 2];
            result.push({
                x: categorical ? state.points[i] + SPREAD_WIDTH * spreadOf(row) : state.points[i],
                y: state.points[i + 1],
                row,
                color: state.pointColors[i / 3] ?? 0,
            });
        }
        return result;
    }, [state.points, state.pointColors, categorical]);
    const most = Math.max(0, ...state.counts);
    const line = fit && mode !== 'boxes' && !categorical ? state.fit : null;

    const columnSpan = (xi: number): [number, number] =>
        categorical ? [xi - CELL_WIDTH / 2, xi + CELL_WIDTH / 2] : [state.xAxis.edges[xi], state.xAxis.edges[xi + 1]];

    const paint = (context: Painter, color: (name: string) => string) => {
        context.font = '12px system-ui, sans-serif';

        // The colours' legend: a swatch and the name with its rows.
        context.textAlign = 'left';
        context.textBaseline = 'middle';
        for (const entry of colorLegend.entries) {
            const x = plot.x + entry.x;
            const y = MARGIN.top + entry.y;
            context.fillStyle = color(groupColorName(groups, entry.group));
            context.fillRect(x, y + 2, SWATCH, SWATCH);
            context.fillStyle = color(hoveredColor === entry.group ? '--accent' : '--fg');
            context.fillText(entry.label, x + SWATCH + 6, y + SWATCH / 2 + 2);
        }

        // Gridlines at the ticks, hairline and recessive; the labels outside.
        context.lineWidth = 1;
        context.fillStyle = color('--muted');
        context.textBaseline = 'middle';
        context.textAlign = 'right';
        for (const tick of state.yAxis.ticks) {
            const y = Math.round(toY(tick.at)) + 0.5;
            context.strokeStyle = color('--chart-grid');
            context.beginPath();
            context.moveTo(plot.x, y);
            context.lineTo(plot.x + plot.w, y);
            context.stroke();
            context.fillText(tick.label, plot.x - 6, y);
        }
        context.textBaseline = 'top';
        context.textAlign = 'center';
        if (categorical) {
            const every = Math.max(1, Math.ceil(90 / (plot.w / state.categories.length)));
            state.categories.forEach((category, i) => {
                if (i % every === 0) context.fillText(category.slice(0, 14), toX(i), plot.y + plot.h + 6);
            });
        } else {
            for (const tick of state.xAxis.ticks) {
                const x = Math.round(toX(tick.at)) + 0.5;
                context.strokeStyle = color('--chart-grid');
                context.beginPath();
                context.moveTo(x, plot.y);
                context.lineTo(x, plot.y + plot.h);
                context.stroke();
                context.fillText(tick.label, x, plot.y + plot.h + 6);
            }
        }
        context.strokeStyle = color('--chart-axis');
        context.strokeRect(plot.x + 0.5, plot.y + 0.5, plot.w - 1, plot.h - 1);

        context.save();
        context.beginPath();
        context.rect(plot.x, plot.y, plot.w, plot.h);
        context.clip();
        if (mode === 'density') {
            const shades = Array.from({ length: STEPS }, (_, i) => color(`--seq-${i + 1}`));
            for (let xi = 0; xi < xBins; xi++) {
                const [left, right] = columnSpan(xi);
                for (let yi = 0; yi < yBins; yi++) {
                    const count = state.counts[xi * yBins + yi] ?? 0;
                    if (count === 0) continue;
                    context.fillStyle = shades[shade(count, most) - 1];
                    const x0 = toX(left);
                    const y0 = toY(state.yAxis.edges[yi + 1]);
                    // A 1px surface gap between touching cells.
                    context.fillRect(x0 + 0.5, y0 + 0.5, toX(right) - x0 - 1, toY(state.yAxis.edges[yi]) - y0 - 1);
                }
            }
        } else if (mode === 'boxes' && categorical) {
            context.strokeStyle = color('--series');
            context.fillStyle = color('--series');
            const half = Math.min(32, (plot.w / state.categories.length) * (BOX_WIDTH / 2));
            state.boxes.forEach((stats, i) => {
                if (stats.count === 0) return;
                const x = toX(i);
                context.globalAlpha = 0.1;
                context.fillRect(x - half, toY(stats.q3), 2 * half, toY(stats.q1) - toY(stats.q3));
                context.globalAlpha = 1;
                context.lineWidth = 2;
                context.strokeRect(x - half, toY(stats.q3), 2 * half, toY(stats.q1) - toY(stats.q3));
                context.beginPath();
                context.moveTo(x - half, toY(stats.median));
                context.lineTo(x + half, toY(stats.median));
                context.moveTo(x, toY(stats.q3));
                context.lineTo(x, toY(stats.highWhisker));
                context.moveTo(x - half / 2, toY(stats.highWhisker));
                context.lineTo(x + half / 2, toY(stats.highWhisker));
                context.moveTo(x, toY(stats.q1));
                context.lineTo(x, toY(stats.lowWhisker));
                context.moveTo(x - half / 2, toY(stats.lowWhisker));
                context.lineTo(x + half / 2, toY(stats.lowWhisker));
                context.stroke();
                for (const outlier of stats.outliers) {
                    context.beginPath();
                    context.arc(x, toY(outlier), 3, 0, 2 * Math.PI);
                    context.fill();
                }
            });
        } else {
            const few = points.length <= FEW_POINTS;
            context.strokeStyle = color('--surface');
            context.lineWidth = 2;
            context.globalAlpha = few ? 1 : 0.5;
            // A colour at a time: switching the fill for each of many points is slow.
            const colours = colored ? state.colors.length : 1;
            for (let group = 0; group < colours; group++) {
                context.fillStyle = color(colored ? groupColorName(groups, group) : '--series');
                for (const point of points) {
                    if (colored && point.color !== group) continue;
                    context.beginPath();
                    context.arc(toX(point.x), toY(point.y), few ? POINT_RADIUS : SMALL_POINT_RADIUS, 0, 2 * Math.PI);
                    if (few) context.stroke();
                    context.fill();
                }
            }
            context.globalAlpha = 1;
        }
        if (line) {
            // Across the plotted range, over the marks.
            context.strokeStyle = color('--fit');
            context.lineWidth = 2;
            context.lineCap = 'round';
            context.beginPath();
            context.moveTo(toX(xLow), toY(line.intercept + line.slope * xLow));
            context.lineTo(toX(xHigh), toY(line.intercept + line.slope * xHigh));
            context.stroke();
        }
        context.restore();

        if (legend && most > 0) {
            // The colour scale: from one row to the most in a cell.
            const x = plot.x + plot.w + 16;
            const step = plot.h / STEPS;
            for (let i = 0; i < STEPS; i++) {
                context.fillStyle = color(`--seq-${i + 1}`);
                context.fillRect(x, plot.y + plot.h - (i + 1) * step + 1, 12, step - 2);
            }
            context.fillStyle = color('--muted');
            context.textAlign = 'left';
            context.textBaseline = 'middle';
            context.fillText(compactCount(most), x + 16, plot.y + 6);
            context.fillText('1', x + 16, plot.y + plot.h - 6);
        }
    };

    useLayoutEffect(() => {
        const element = canvas.current;
        const context = element?.getContext('2d');
        if (!element || !context) return;
        const ratio = window.devicePixelRatio || 1;
        element.width = Math.round(width * ratio);
        element.height = Math.round(height * ratio);
        context.setTransform(ratio, 0, 0, ratio, 0, 0);
        context.clearRect(0, 0, width, height);
        const style = getComputedStyle(element);
        paint(context, (name) => style.getPropertyValue(name).trim());
    });

    useImperativeHandle(ref, () => ({
        image: () => {
            const element = canvas.current;
            if (!element) return null;
            const style = getComputedStyle(element);
            const svg = new SvgContext(width, height, surfaceColor(element));
            paint(svg, (name) => style.getPropertyValue(name).trim());
            return { svg: svg.toString(), width, height };
        },
    }));

    // What lies under the pointer: a point, a density cell or a category.
    const find = (px: number, py: number): { pick: ScatterPick; value: string; label: string } | null => {
        const entry = colorLegend.entries.find(
            (e) =>
                px >= plot.x + e.x &&
                px <= plot.x + e.x + e.width &&
                py >= MARGIN.top + e.y &&
                py < MARGIN.top + e.y + LEGEND_LINE,
        );
        if (entry) {
            const rows = state.colorPairs[entry.group] ?? 0;
            return {
                pick: { kind: 'color', color: entry.group },
                value: `${rows.toLocaleString()} ${rows === 1 ? 'row' : 'rows'}`,
                label: `${state.colors[entry.group]} · click to filter to it`,
            };
        }
        if (px < plot.x || px > plot.x + plot.w || py < plot.y || py > plot.y + plot.h) return null;
        if (mode === 'boxes' && categorical) {
            const category = Math.round(fromX(px));
            if (category < 0 || category >= state.categories.length) return null;
            const name = state.categories[category];
            const text = state.boxTexts[category] ?? '';
            // The text starts with the category, which the tooltip shows above it.
            const label = text.startsWith(`${name}: `) ? text.slice(name.length + 2) : text;
            return { pick: { kind: 'category', category }, value: name, label };
        }
        if (mode === 'density') {
            const x = fromX(px);
            const y = fromY(py);
            let xi = -1;
            for (let i = 0; i < xBins; i++) {
                const [left, right] = columnSpan(i);
                if (x >= left && x < right) xi = i;
            }
            const yi = state.yAxis.edges.findIndex((edge, i) => i < yBins && y >= edge && y < state.yAxis.edges[i + 1]);
            if (xi < 0 || yi < 0) return null;
            const count = state.counts[xi * yBins + yi] ?? 0;
            const xText = categorical ? state.categories[xi] : state.xAxis.binRanges[xi];
            return {
                pick: { kind: 'cell', xBin: xi, yBin: yi },
                value: `${count.toLocaleString()} rows`,
                label: `${xText} × ${state.yAxis.binRanges[yi]}`,
            };
        }
        let best = -1;
        let bestDistance = HIT_RADIUS * HIT_RADIUS;
        points.forEach((point, i) => {
            const dx = toX(point.x) - px;
            const dy = toY(point.y) - py;
            const distance = dx * dx + dy * dy;
            if (distance <= bestDistance) {
                best = i;
                bestDistance = distance;
            }
        });
        if (best < 0) return null;
        const point = points[best];
        const coordinate = (axis: ScatterAxis, value: number) => (axis.kind === 'number' ? numberText(value) : '');
        const xText = categorical ? state.categories[Math.round(point.x)] : coordinate(state.xAxis, point.x);
        const yText = coordinate(state.yAxis, point.y);
        return {
            pick: { kind: 'row', row: point.row },
            value: `Row ${(point.row + 1).toLocaleString()}`,
            label:
                (colored ? `${state.colors[point.color] ?? ''} · ` : '') +
                (xText && yText ? `${xText}, ${yText} · click to go to the row` : 'Click to go to the row'),
        };
    };

    const position = (event: React.PointerEvent) => {
        const rect = event.currentTarget.getBoundingClientRect();
        return { x: event.clientX - rect.left, y: event.clientY - rect.top };
    };
    const draggable = !categorical && mode !== 'boxes';

    return (
        <div ref={box} className="relative w-full select-none">
            <canvas
                ref={canvas}
                id="scatter-chart"
                role="img"
                aria-label={state.summary}
                className="block cursor-crosshair"
                data-fit={line ? 'true' : undefined}
                style={{ width, height }}
                onPointerDown={(event) => {
                    if (event.button !== 0) return;
                    press.current = position(event);
                    event.currentTarget.setPointerCapture(event.pointerId);
                }}
                onPointerMove={(event) => {
                    const at = position(event);
                    const start = press.current;
                    if (
                        start &&
                        draggable &&
                        (Math.abs(at.x - start.x) > DRAG_THRESHOLD || Math.abs(at.y - start.y) > DRAG_THRESHOLD)
                    ) {
                        const clamp = (value: number, low: number, size: number) =>
                            Math.min(low + size, Math.max(low, value));
                        setBand({
                            x0: start.x,
                            y0: start.y,
                            x1: clamp(at.x, plot.x, plot.w),
                            y1: clamp(at.y, plot.y, plot.h),
                        });
                        setHover(null);
                        return;
                    }
                    const found = find(at.x, at.y);
                    setHover(found && { x: at.x, y: at.y, value: found.value, label: found.label });
                    setHoveredColor(found?.pick.kind === 'color' ? found.pick.color : null);
                }}
                onPointerUp={(event) => {
                    const at = position(event);
                    press.current = null;
                    if (band) {
                        setBand(null);
                        const xs = [fromX(band.x0), fromX(band.x1)];
                        const ys = [fromY(band.y0), fromY(band.y1)];
                        onPick({
                            kind: 'range',
                            xLow: Math.min(...xs),
                            xHigh: Math.max(...xs),
                            yLow: Math.min(...ys),
                            yHigh: Math.max(...ys),
                        });
                        return;
                    }
                    const found = find(at.x, at.y);
                    if (found) onPick(found.pick);
                }}
                onPointerLeave={() => {
                    setHover(null);
                    setHoveredColor(null);
                }}
            />
            {band && (
                <div
                    className="pointer-events-none absolute border border-accent bg-selection"
                    style={{
                        left: Math.min(band.x0, band.x1),
                        top: Math.min(band.y0, band.y1),
                        width: Math.abs(band.x1 - band.x0),
                        height: Math.abs(band.y1 - band.y0),
                    }}
                />
            )}
            {hover && <Tooltip x={hover.x} y={hover.y - 6} value={hover.value} label={hover.label} />}
        </div>
    );
}
