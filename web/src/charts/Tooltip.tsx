import type { ReactNode } from 'react';

/** The hover readout: the value leads, the label follows. */
// Above the point (below it near the top, where it would be cut off).
export function Tooltip({ x, y, value, label }: { x: number; y: number; value: ReactNode; label: ReactNode }) {
    const below = y < 64;
    return (
        <div
            role="tooltip"
            className={
                'chart-tooltip pointer-events-none absolute z-10 w-max max-w-80 -translate-x-1/2 rounded border border-line ' +
                `bg-surface px-2 py-1 shadow-md ${below ? '' : '-translate-y-full'}`
            }
            style={{ left: x, top: below ? y + 12 : y - 8 }}
        >
            <div className="font-semibold tabular-nums">{value}</div>
            <div className="text-muted">{label}</div>
        </div>
    );
}
