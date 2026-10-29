import { type ReactNode, useRef } from 'react';

export type SidebarTab = 'content' | 'filter' | 'stats';

interface SidebarProps {
    tab: SidebarTab;
    onTab: (tab: SidebarTab) => void;
    width: number;
    onWidth: (width: number) => void;
    onClose: () => void;
    children: ReactNode; // the tab's pane
}

const TABS: { tab: SidebarTab; label: string }[] = [
    { tab: 'content', label: 'Content' },
    { tab: 'filter', label: 'Filter' },
    { tab: 'stats', label: 'Statistics' },
];
export const SIDEBAR_MIN_WIDTH = 200;

/** The panes beside the grid, as in the GUI: its left edge is dragged to widen it. */
export function Sidebar({ tab, onTab, width, onWidth, onClose, children }: SidebarProps) {
    const drag = useRef<{ x: number; width: number } | null>(null);
    return (
        <aside
            id="sidebar"
            className="relative flex shrink-0 flex-col border-l border-line bg-surface"
            style={{ width }}
        >
            <div
                className="absolute inset-y-0 -left-1 z-10 w-2 cursor-col-resize"
                role="separator"
                aria-orientation="vertical"
                aria-label="Resize the sidebar"
                onPointerDown={(event) => {
                    drag.current = { x: event.clientX, width };
                    event.currentTarget.setPointerCapture(event.pointerId);
                }}
                onPointerMove={(event) => {
                    const start = drag.current;
                    if (!start) return;
                    const most = Math.max(SIDEBAR_MIN_WIDTH, window.innerWidth - 240);
                    onWidth(Math.min(most, Math.max(SIDEBAR_MIN_WIDTH, start.width + start.x - event.clientX)));
                }}
                onPointerUp={() => (drag.current = null)}
            />
            <header className="flex items-center gap-1 border-b border-line bg-chrome px-1.5 py-1" role="tablist">
                {TABS.map((item) => (
                    <button
                        key={item.tab}
                        type="button"
                        role="tab"
                        id={`sidebar-${item.tab}`}
                        aria-selected={tab === item.tab}
                        className={`rounded px-2 py-0.5 ${tab === item.tab ? 'bg-surface font-semibold' : 'text-muted hover:text-fg'}`}
                        onClick={() => onTab(item.tab)}
                    >
                        {item.label}
                    </button>
                ))}
                <span className="flex-1" />
                <button
                    type="button"
                    id="sidebar-close"
                    className="px-1 text-muted hover:text-fg"
                    aria-label="Close the sidebar"
                    onClick={onClose}
                >
                    ✕
                </button>
            </header>
            {children}
        </aside>
    );
}
