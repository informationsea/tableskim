import { type Ref, useEffect, useImperativeHandle, useRef, useState } from 'react';
import type { Histogram } from '../protocol';
import { groupColor } from './groups';
import { type ChartImage, svgOfElement } from './image';
import { barPath, columnPath, compactCount, niceTicks, observeWidth } from './scale';
import { Tooltip } from './Tooltip';

interface HistogramChartProps {
    histogram: Histogram;
    rows: number; // in the view, for the shares
    // A bar (or a frequent value) was clicked; `group`: a part of a stacked bar.
    onPick: (bin: number, group?: number) => void;
    ref?: Ref<ChartImage>;
}

const HEIGHT = 320;
const MARGIN = { top: 12, right: 12, bottom: 44, left: 52 };
const BAR_THICKNESS = 20;
const ROW_GAP = 8;
const LABEL_WIDTH = 160;

const LEGEND_LINE = 18;
const SWATCH = 10;

/** The legend's entries placed on lines of `width`, with the height they take. */
function legendLayout(groups: string[], width: number) {
    const entries: { x: number; y: number; label: string }[] = [];
    let x = 0;
    let y = 0;
    for (const group of groups) {
        const label = group.length > 24 ? `${group.slice(0, 23)}…` : group;
        const entry = SWATCH + 6 + label.length * 7 + 14;
        if (x > 0 && x + entry > width) {
            x = 0;
            y += LEGEND_LINE;
        }
        entries.push({ x, y, label });
        x += entry;
    }
    return { entries, height: groups.length > 0 ? y + LEGEND_LINE + 6 : 0 };
}

function share(count: number, rows: number) {
    return rows > 0 ? ` (${((100 * count) / rows).toLocaleString(undefined, { maximumFractionDigits: 1 })}%)` : '';
}

/** Columns of the bins' counts, or bars of the most frequent values. */
export function HistogramChart({ histogram, rows, onPick, ref }: HistogramChartProps) {
    const box = useRef<HTMLDivElement>(null);
    const chart = useRef<SVGSVGElement>(null);
    useImperativeHandle(ref, () => ({
        image: () => {
            const svg = chart.current;
            return svg
                ? { svg: svgOfElement(svg), width: svg.width.baseVal.value, height: svg.height.baseVal.value }
                : null;
        },
    }));
    const [width, setWidth] = useState(600);
    const [hover, setHover] = useState<{ index: number; group?: number; x: number; y: number } | null>(null);
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
    const part = hover?.group !== undefined ? bins[hover.index]?.groups?.[hover.group] : undefined;
    const tooltip =
        hovered &&
        hover &&
        (part !== undefined && hover.group !== undefined ? (
            <Tooltip
                x={hover.x}
                y={hover.y}
                value={`${part.toLocaleString()} ${part === 1 ? 'row' : 'rows'}${share(part, hovered.count)} of the bin`}
                label={`${histogram.groups[hover.group]} · ${hovered.range}`}
            />
        ) : (
            <Tooltip
                x={hover.x}
                y={hover.y}
                value={`${hovered.count.toLocaleString()} rows${share(hovered.count, rows)}`}
                label={hovered.range || '(empty)'}
            />
        ));

    if (bins.length === 0) {
        // Text: the most frequent values, as bars from a common baseline.
        const plotWidth = Math.max(40, width - LABEL_WIDTH - 72);
        const height = items.length * (BAR_THICKNESS + ROW_GAP);
        return (
            <div ref={box} className="relative w-full">
                <svg
                    ref={chart}
                    id="histogram-chart"
                    width={width}
                    height={height}
                    role="img"
                    aria-label="Most frequent values"
                >
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
    const legend = legendLayout(histogram.groups, plotWidth);
    const plotTop = MARGIN.top + legend.height;
    const plotHeight = HEIGHT - MARGIN.top - MARGIN.bottom;
    const ticks = niceTicks(0, most);
    const top = ticks[ticks.length - 1];
    const toY = (count: number) => plotTop + plotHeight - (plotHeight * count) / top;
    const bottom = plotTop + plotHeight;
    // The parts of a stacked bar, from the bottom: running totals, so that
    // the bar keeps its height.
    const parts = (counts: number[]) => {
        let sum = 0;
        return counts.map((count, group) => {
            const below = toY(sum);
            sum += count;
            return { group, count, y: toY(sum), height: below - toY(sum) };
        });
    };
    // The part of a stacked bar under the pointer.
    const partAt = (event: React.MouseEvent, counts: number[] | undefined) => {
        const svg = chart.current?.getBoundingClientRect();
        if (!counts || !svg) return undefined;
        const y = event.clientY - svg.top;
        return parts(counts).find((p) => p.count > 0 && y >= p.y && y <= p.y + p.height);
    };
    const slot = plotWidth / bins.length;
    // Every k-th bin is labelled, so that the labels do not collide.
    const longest = Math.max(...bins.map((bin) => bin.label.length));
    const every = Math.max(1, Math.ceil((longest * 7 + 12) / slot));
    return (
        <div ref={box} className="relative w-full">
            <svg
                ref={chart}
                id="histogram-chart"
                width={width}
                height={HEIGHT + legend.height}
                role="img"
                aria-label="Histogram"
            >
                {legend.entries.length > 0 && (
                    <g id="histogram-legend" aria-label="Groups">
                        {legend.entries.map((entry, group) => (
                            <g key={group} transform={`translate(${MARGIN.left + entry.x},${MARGIN.top + entry.y})`}>
                                <rect
                                    y={2}
                                    width={SWATCH}
                                    height={SWATCH}
                                    rx={2}
                                    style={{ fill: groupColor(histogram, group) }}
                                />
                                <text x={SWATCH + 6} y={SWATCH / 2 + 2} dy="0.35em" className="fill-fg text-xs">
                                    {entry.label}
                                </text>
                            </g>
                        ))}
                    </g>
                )}
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
                            onClick={(event) => onPick(i, partAt(event, bin.groups)?.group)}
                            onKeyDown={(event) => keyDown(event, i)}
                            onPointerMove={(event) => {
                                const under = partAt(event, bin.groups);
                                setHover({ index: i, group: under?.group, x: x + slot / 2, y: under?.y ?? barTop });
                            }}
                            onFocus={() => setHover({ index: i, x: x + slot / 2, y: barTop })}
                            onPointerLeave={() => setHover(null)}
                            onBlur={() => setHover(null)}
                        >
                            {/* The whole slot answers the pointer, not only the painted bar. */}
                            <rect x={x} y={plotTop} width={slot} height={plotHeight} fill="transparent" />
                            {bin.groups &&
                                parts(bin.groups).map(
                                    (p) =>
                                        p.count > 0 && (
                                            <rect
                                                key={p.group}
                                                className="segment"
                                                x={x + 1}
                                                y={p.y}
                                                width={barWidth}
                                                // A 2px gap in the surface colour between parts.
                                                height={Math.max(1, p.height - (p.y + p.height < bottom ? 2 : 0))}
                                                style={{ fill: groupColor(histogram, p.group) }}
                                                opacity={
                                                    hover?.index === i &&
                                                    hover.group !== undefined &&
                                                    hover.group !== p.group
                                                        ? 0.55
                                                        : 1
                                                }
                                            />
                                        ),
                                )}
                            {!bin.groups && bin.count > 0 && (
                                <path
                                    d={columnPath(x + 1, barTop, barWidth, bottom - barTop)}
                                    className={hover?.index === i ? 'fill-series-hover' : 'fill-series'}
                                />
                            )}
                            {i % every === 0 && (
                                <text
                                    x={x + slot / 2}
                                    y={bottom + 16}
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
