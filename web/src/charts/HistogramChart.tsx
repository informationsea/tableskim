import { useEffect, useRef, useState } from 'react';
import type { Histogram } from '../protocol';
import { barPath, columnPath, compactCount, niceTicks, observeWidth } from './scale';
import { Tooltip } from './Tooltip';

interface HistogramChartProps {
    histogram: Histogram;
    rows: number; // in the view, for the shares
    onPick: (bin: number) => void; // a bar (or a frequent value) was clicked
}

const HEIGHT = 320;
const MARGIN = { top: 12, right: 12, bottom: 44, left: 52 };
const BAR_THICKNESS = 20;
const ROW_GAP = 8;
const LABEL_WIDTH = 160;

function share(count: number, rows: number) {
    return rows > 0 ? ` (${((100 * count) / rows).toLocaleString(undefined, { maximumFractionDigits: 1 })}%)` : '';
}

/** Columns of the bins' counts, or bars of the most frequent values. */
export function HistogramChart({ histogram, rows, onPick }: HistogramChartProps) {
    const box = useRef<HTMLDivElement>(null);
    const [width, setWidth] = useState(600);
    const [hover, setHover] = useState<{ index: number; x: number; y: number } | null>(null);
    useEffect(() => (box.current ? observeWidth(box.current, setWidth) : undefined), []);

    const bins = histogram.bins;
    const items =
        bins.length > 0
            ? bins
            : histogram.top.map((value) => ({ label: value.value, range: value.value, count: value.count }));
    const most = Math.max(1, ...items.map((item) => item.count));

    const keyDown = (event: React.KeyboardEvent, index: number) => {
        if (event.key === 'Enter' || event.key === ' ') {
            event.preventDefault();
            onPick(index);
        }
    };
    const hovered = hover && items[hover.index];
    const tooltip = hovered && hover && (
        <Tooltip
            x={hover.x}
            y={hover.y}
            value={`${hovered.count.toLocaleString()} rows${share(hovered.count, rows)}`}
            label={hovered.range || '(empty)'}
        />
    );

    if (bins.length === 0) {
        // Text: the most frequent values, as bars from a common baseline.
        const plotWidth = Math.max(40, width - LABEL_WIDTH - 72);
        const height = items.length * (BAR_THICKNESS + ROW_GAP);
        return (
            <div ref={box} className="relative w-full">
                <svg id="histogram-chart" width={width} height={height} role="img" aria-label="Most frequent values">
                    {items.map((item, i) => {
                        const y = i * (BAR_THICKNESS + ROW_GAP);
                        const length = Math.max(1, (plotWidth * item.count) / most);
                        return (
                            <g
                                key={i}
                                className="bar cursor-pointer outline-none"
                                tabIndex={0}
                                aria-label={`${item.label || '(empty)'}: ${item.count}`}
                                onClick={() => onPick(i)}
                                onKeyDown={(event) => keyDown(event, i)}
                                onPointerMove={() => setHover({ index: i, x: LABEL_WIDTH + length, y })}
                                onFocus={() => setHover({ index: i, x: LABEL_WIDTH + length, y })}
                                onPointerLeave={() => setHover(null)}
                                onBlur={() => setHover(null)}
                            >
                                <rect
                                    x={0}
                                    y={y - ROW_GAP / 2}
                                    width={width}
                                    height={BAR_THICKNESS + ROW_GAP}
                                    fill="transparent"
                                />
                                <text
                                    x={LABEL_WIDTH - 8}
                                    y={y + BAR_THICKNESS / 2}
                                    dy="0.35em"
                                    textAnchor="end"
                                    className="fill-fg"
                                >
                                    {(item.label || '(empty)').slice(0, 24)}
                                </text>
                                <path
                                    d={barPath(LABEL_WIDTH, y, length, BAR_THICKNESS)}
                                    className={hover?.index === i ? 'fill-series-hover' : 'fill-series'}
                                />
                                <text
                                    x={LABEL_WIDTH + length + 6}
                                    y={y + BAR_THICKNESS / 2}
                                    dy="0.35em"
                                    className="fill-muted tabular-nums"
                                >
                                    {item.count.toLocaleString()}
                                </text>
                            </g>
                        );
                    })}
                </svg>
                {tooltip}
            </div>
        );
    }

    const plotWidth = Math.max(40, width - MARGIN.left - MARGIN.right);
    const plotHeight = HEIGHT - MARGIN.top - MARGIN.bottom;
    const ticks = niceTicks(0, most);
    const top = ticks[ticks.length - 1];
    const toY = (count: number) => MARGIN.top + plotHeight - (plotHeight * count) / top;
    const slot = plotWidth / bins.length;
    // Every k-th bin is labelled, so that the labels do not collide.
    const longest = Math.max(...bins.map((bin) => bin.label.length));
    const every = Math.max(1, Math.ceil((longest * 7 + 12) / slot));
    return (
        <div ref={box} className="relative w-full">
            <svg id="histogram-chart" width={width} height={HEIGHT} role="img" aria-label="Histogram">
                {ticks.map((tick) => (
                    <g key={tick}>
                        <line
                            x1={MARGIN.left}
                            x2={MARGIN.left + plotWidth}
                            y1={toY(tick)}
                            y2={toY(tick)}
                            className={tick === 0 ? 'stroke-chart-axis' : 'stroke-chart-grid'}
                            strokeWidth={1}
                            shapeRendering="crispEdges"
                        />
                        <text
                            x={MARGIN.left - 6}
                            y={toY(tick)}
                            dy="0.35em"
                            textAnchor="end"
                            className="fill-muted text-xs tabular-nums"
                        >
                            {compactCount(tick)}
                        </text>
                    </g>
                ))}
                {bins.map((bin, i) => {
                    const x = MARGIN.left + i * slot;
                    const barTop = toY(bin.count);
                    // A 2px gap in the surface colour between touching columns.
                    const barWidth = Math.max(1, slot - 2);
                    return (
                        <g
                            key={i}
                            className="bar cursor-pointer outline-none"
                            tabIndex={0}
                            aria-label={`${bin.range}: ${bin.count}`}
                            onClick={() => onPick(i)}
                            onKeyDown={(event) => keyDown(event, i)}
                            onPointerMove={() => setHover({ index: i, x: x + slot / 2, y: barTop })}
                            onFocus={() => setHover({ index: i, x: x + slot / 2, y: barTop })}
                            onPointerLeave={() => setHover(null)}
                            onBlur={() => setHover(null)}
                        >
                            {/* The whole slot answers the pointer, not only the painted bar. */}
                            <rect x={x} y={MARGIN.top} width={slot} height={plotHeight} fill="transparent" />
                            {bin.count > 0 && (
                                <path
                                    d={columnPath(x + 1, barTop, barWidth, MARGIN.top + plotHeight - barTop)}
                                    className={hover?.index === i ? 'fill-series-hover' : 'fill-series'}
                                />
                            )}
                            {i % every === 0 && (
                                <text
                                    x={x + slot / 2}
                                    y={MARGIN.top + plotHeight + 16}
                                    textAnchor="middle"
                                    className="fill-muted text-xs tabular-nums"
                                >
                                    {bin.label}
                                </text>
                            )}
                        </g>
                    );
                })}
            </svg>
            {tooltip}
        </div>
    );
}
