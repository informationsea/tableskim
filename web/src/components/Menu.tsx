import { useEffect, useLayoutEffect, useRef, useState } from 'react';

export type MenuItem = { label: string; onSelect: () => void; disabled?: boolean } | 'separator';

interface MenuProps {
    x: number;
    y: number;
    items: MenuItem[];
    onClose: () => void;
}

/** A context menu at a point; closes on a choice, Escape or a click elsewhere. */
export function Menu({ x, y, items, onClose }: MenuProps) {
    const menu = useRef<HTMLDivElement>(null);
    const [position, setPosition] = useState({ left: x, top: y });

    // Kept inside the window.
    useLayoutEffect(() => {
        const element = menu.current;
        if (!element) return;
        const { width, height } = element.getBoundingClientRect();
        setPosition({
            left: Math.max(0, Math.min(x, window.innerWidth - width - 4)),
            top: Math.max(0, Math.min(y, window.innerHeight - height - 4)),
        });
        element.querySelector<HTMLElement>('button:not(:disabled)')?.focus();
    }, [x, y]);

    useEffect(() => {
        const down = (event: MouseEvent) => {
            if (!menu.current?.contains(event.target as Node)) onClose();
        };
        window.addEventListener('mousedown', down);
        window.addEventListener('blur', onClose);
        return () => {
            window.removeEventListener('mousedown', down);
            window.removeEventListener('blur', onClose);
        };
    }, [onClose]);

    const keyDown = (event: React.KeyboardEvent) => {
        const buttons = [...(menu.current?.querySelectorAll<HTMLElement>('button:not(:disabled)') ?? [])];
        const index = buttons.indexOf(document.activeElement as HTMLElement);
        if (event.key === 'Escape') onClose();
        else if (event.key === 'ArrowDown') buttons[(index + 1) % buttons.length]?.focus();
        else if (event.key === 'ArrowUp') buttons[(index - 1 + buttons.length) % buttons.length]?.focus();
        else return;
        event.preventDefault();
    };

    return (
        <div
            ref={menu}
            role="menu"
            id="context-menu"
            className="fixed z-50 min-w-48 rounded-md border border-line bg-surface py-1 shadow-lg"
            style={position}
            onKeyDown={keyDown}
        >
            {items.map((item, i) =>
                item === 'separator' ? (
                    <div key={i} className="my-1 border-t border-line" />
                ) : (
                    <button
                        key={i}
                        type="button"
                        role="menuitem"
                        className="block w-full px-3 py-1 text-left hover:bg-active focus:bg-active focus:outline-none disabled:opacity-40"
                        disabled={item.disabled}
                        onClick={() => {
                            onClose();
                            item.onSelect();
                        }}
                    >
                        {item.label}
                    </button>
                ),
            )}
        </div>
    );
}
