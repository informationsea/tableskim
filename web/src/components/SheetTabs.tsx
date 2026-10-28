interface SheetTabsProps {
    sheets: string[];
    current: string | null;
    onSelect: (name: string) => void;
}

export function SheetTabs({ sheets, current, onSelect }: SheetTabsProps) {
    return (
        <nav
            id="tabs"
            role="tablist"
            className="flex gap-0.5 overflow-x-auto border-b border-line bg-chrome px-2 pt-1"
            hidden={sheets.length < 2}
        >
            {sheets.map((name) => (
                <button
                    key={name}
                    type="button"
                    role="tab"
                    aria-selected={name === current}
                    className={
                        'control rounded-b-none border-b-0 whitespace-nowrap ' +
                        (name === current ? 'border-accent font-semibold' : 'bg-chrome')
                    }
                    onClick={() => onSelect(name)}
                >
                    {name}
                </button>
            ))}
        </nav>
    );
}
