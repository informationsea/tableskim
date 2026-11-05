import type { Histogram } from '../protocol';

/** Groups of a column's values told apart by colour: their names, then "(other)" and "(empty)" if flagged. */
export interface ValueGroups {
    names: string[];
    other: boolean; // the names end with "(other)"
    empty: boolean; // ... and then "(empty)"
}

export const histogramGroups = (histogram: Histogram): ValueGroups => ({
    names: histogram.groups,
    other: histogram.otherGroup,
    empty: histogram.emptyGroup,
});

/** A group's colour token: the categorical slots in order, grays for "(other)" and "(empty)". */
export function groupColorName(groups: ValueGroups, group: number) {
    const count = groups.names.length;
    if (groups.empty && group === count - 1) return '--cat-empty';
    if (groups.other && group === count - (groups.empty ? 2 : 1)) return '--cat-other';
    return `--cat-${(group % 8) + 1}`;
}

export function groupColor(histogram: Histogram, group: number) {
    return `var(${groupColorName(histogramGroups(histogram), group)})`;
}

export const LEGEND_LINE = 18;
export const SWATCH = 10;

/** Legend entries placed on lines of `width`, with the height they take. */
export function legendLayout(entries: { group: number; label: string }[], width: number) {
    const placed: { x: number; y: number; width: number; label: string; group: number }[] = [];
    let x = 0;
    let y = 0;
    for (const { group, label: name } of entries) {
        const label = name.length > 24 ? `${name.slice(0, 23)}…` : name;
        const entry = SWATCH + 6 + label.length * 7 + 14;
        if (x > 0 && x + entry > width) {
            x = 0;
            y += LEGEND_LINE;
        }
        placed.push({ x, y, width: entry - 14, label, group });
        x += entry;
    }
    return { entries: placed, height: placed.length > 0 ? y + LEGEND_LINE + 6 : 0 };
}
