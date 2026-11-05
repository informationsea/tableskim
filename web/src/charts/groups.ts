import type { Histogram } from '../protocol';

/** A group's colour: the categorical slots in order, grays for "(other)" and "(empty)". */
export function groupColor(histogram: Histogram, group: number) {
    const count = histogram.groups.length;
    if (histogram.emptyGroup && group === count - 1) return 'var(--cat-empty)';
    if (histogram.otherGroup && group === count - (histogram.emptyGroup ? 2 : 1)) return 'var(--cat-other)';
    return `var(--cat-${(group % 8) + 1})`;
}
