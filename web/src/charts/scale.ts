// Axis helpers shared by the charts.

/** About `count` round ticks from 0 (or `low`) to at least `high`. */
export function niceTicks(low: number, high: number, count = 5): number[] {
    if (!(high > low)) return [low];
    const raw = (high - low) / count;
    const magnitude = 10 ** Math.floor(Math.log10(raw));
    const step = [1, 2, 2.5, 5, 10].map((f) => f * magnitude).find((s) => s >= raw) ?? raw;
    const ticks = [];
    for (let tick = Math.floor(low / step) * step; tick <= high + step * 1e-9; tick += step) ticks.push(tick);
    if (ticks[ticks.length - 1] < high) ticks.push(ticks[ticks.length - 1] + step);
    return ticks;
}

const compact = new Intl.NumberFormat(undefined, { notation: 'compact', maximumFractionDigits: 1 });

/** 0 / 950 / 1.2K / 3M: counts on an axis. */
export function compactCount(value: number): string {
    return Math.abs(value) < 1000 ? value.toLocaleString() : compact.format(value);
}

/** A bar growing up from the baseline: 4px rounded at the data end, square at the base. */
export function columnPath(x: number, y: number, width: number, height: number): string {
    const r = Math.min(4, width / 2, height);
    return (
        `M${x},${y + height}V${y + r}Q${x},${y} ${x + r},${y}` +
        `H${x + width - r}Q${x + width},${y} ${x + width},${y + r}V${y + height}Z`
    );
}

/** A bar growing right from the baseline, rounded at its end. */
export function barPath(x: number, y: number, width: number, height: number): string {
    const r = Math.min(4, height / 2, width);
    return (
        `M${x},${y}H${x + width - r}Q${x + width},${y} ${x + width},${y + r}` +
        `V${y + height - r}Q${x + width},${y + height} ${x + width - r},${y + height}H${x}Z`
    );
}

/** The width of an element, followed as it changes. */
export function observeWidth(element: HTMLElement, set: (width: number) => void): () => void {
    const observer = new ResizeObserver(() => set(element.clientWidth));
    observer.observe(element);
    set(element.clientWidth);
    return () => observer.disconnect();
}
