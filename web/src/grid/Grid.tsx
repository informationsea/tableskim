// Draws the cells GridController has in view. The controller keeps the
// state that changes with every scroll; a redraw is one render per frame.

import { useEffect, useImperativeHandle, useLayoutEffect, useReducer, useRef, useState, type Ref } from 'react';
import type { SheetInfo } from '../protocol';
import { type Cell, type GridCallbacks, GridController, HEADER_HEIGHT, ROW_HEIGHT } from './controller';

export interface GridHandle {
    select: (cell: Cell) => void;
    cursor: () => Cell;
    focus: () => void;
}

interface GridProps extends GridCallbacks {
    info: SheetInfo | null;
    // Changes when another sheet (or file) is shown.
    sheetKey: number;
    // Bumped when rows were loaded, and when the view changed what each row is.
    dataVersion: number;
    viewVersion: number;
    ref?: Ref<GridHandle>;
}

const CELL = 'cell absolute h-6 truncate border-r border-b border-grid-line px-1.5 leading-[23px]';
const KIND_CLASSES: Record<string, string> = {
    n: 'k-n text-right tabular-nums',
    s: 'k-s',
    e: 'k-e',
    l: 'k-l',
    o: 'k-o',
    x: 'k-x text-danger',
};

export function Grid({ info, sheetKey, dataVersion, viewVersion, ref, ...callbacks }: GridProps) {
    const scrollerRef = useRef<HTMLDivElement>(null);
    const layerRef = useRef<HTMLDivElement>(null);
    const spacerRef = useRef<HTMLDivElement>(null);
    const [, redrawNow] = useReducer((frame: number) => frame + 1, 0);
    const [controller] = useState(() => new GridController(redrawNow));
    const shownSheet = useRef<number | null>(null);

    // First, so that the effects below already call the latest callbacks.
    useLayoutEffect(() => controller.setCallbacks(callbacks));

    useImperativeHandle(
        ref,
        () => ({
            select: (cell) => controller.select(cell),
            cursor: () => controller.cursor,
            focus: () => scrollerRef.current?.focus(),
        }),
        [controller],
    );

    useLayoutEffect(() => {
        controller.attach(scrollerRef.current, layerRef.current, spacerRef.current);
        const observer = new ResizeObserver(() => {
            controller.updateExtent();
            redrawNow();
        });
        if (scrollerRef.current) observer.observe(scrollerRef.current);
        return () => {
            observer.disconnect();
            controller.detach();
        };
    }, [controller]);

    useLayoutEffect(() => {
        if (!info) {
            controller.clear();
            shownSheet.current = null;
            return;
        }
        controller.setInfo(info, shownSheet.current !== sheetKey);
        shownSheet.current = sheetKey;
    }, [controller, info, sheetKey]);

    useEffect(() => {
        if (dataVersion) controller.dataChanged();
    }, [controller, dataVersion]);

    useEffect(() => {
        if (viewVersion) controller.viewChanged();
    }, [controller, viewVersion]);

    const view = controller.viewport();
    // After each render: the extent may have changed, and rows may be missing.
    useLayoutEffect(() => {
        controller.updateExtent();
        controller.fetchIfNeeded(view);
    });

    const selection = controller.selection();
    const { cursor, columns, widths, sort, rowNumberWidth } = controller;
    const cells = [];
    const rowNumbers = [];
    const headers = [];
    if (columns.length > 0) {
        let x = rowNumberWidth - view.columnShift;
        for (let column = view.firstColumn; column <= view.lastColumn; column++) {
            const width = widths[column];
            const inColumns = column >= selection.left && column <= selection.right;
            for (let row = view.firstRow; row <= view.lastRow; row++) {
                const y = HEADER_HEIGHT + (row - view.firstRow) * ROW_HEIGHT - view.rowShift;
                const { kind, text } = controller.cell(row, column);
                const selected = inColumns && row >= selection.top && row <= selection.bottom;
                const isCursor = row === cursor.row && column === cursor.column;
                cells.push(
                    <div
                        key={`${row}:${column}`}
                        className={
                            `${CELL} ${KIND_CLASSES[kind] ?? ''}${selected ? 'selected bg-selection' : ''}` +
                            (isCursor ? ' cursor shadow-[inset_0_0_0_2px_var(--color-accent)]' : '')
                        }
                        data-row={row}
                        data-column={column}
                        style={{ left: x, top: y, width }}
                    >
                        {kind === 'l' ? (
                            <span className="inline-block h-1.5 w-3/5 rounded-full bg-loading align-middle" />
                        ) : (
                            text
                        )}
                    </div>,
                );
            }
            x += width;
        }
        for (let row = view.firstRow; row <= view.lastRow; row++) {
            const y = HEADER_HEIGHT + (row - view.firstRow) * ROW_HEIGHT - view.rowShift;
            const active = row >= selection.top && row <= selection.bottom;
            rowNumbers.push(
                <div
                    key={row}
                    className={
                        'row-number absolute left-0 z-1 h-6 truncate border-r border-b border-grid-line px-1.5 text-right ' +
                        `leading-[23px] text-muted tabular-nums ${active ? 'active bg-active' : 'bg-header'}`
                    }
                    data-row={row}
                    style={{ top: y, width: rowNumberWidth }}
                >
                    {row + 1}
                </div>,
            );
        }
        x = rowNumberWidth - view.columnShift;
        for (let column = view.firstColumn; column <= view.lastColumn; column++) {
            const width = widths[column];
            const { name, mark } = columns[column];
            const sortIndex = sort.findIndex((key) => key.column === column);
            const sortMark =
                sortIndex < 0
                    ? ''
                    : (sort[sortIndex].ascending ? '▲' : '▼') + (sort.length > 1 ? String(sortIndex + 1) : '');
            const active = column >= selection.left && column <= selection.right;
            const filtered = controller.filteredColumns.has(column);
            headers.push(
                <div
                    key={column}
                    className={
                        'header absolute top-0 z-2 h-7 cursor-pointer truncate border-r border-b border-grid-line px-1.5 ' +
                        `leading-[27px] font-semibold ${active ? 'active bg-active' : 'bg-header'}${filtered ? ' filtered' : ''}`
                    }
                    data-column={column}
                    title={name}
                    style={{ left: x, width }}
                >
                    {mark && <span className="mark mr-1 font-normal text-muted">{mark}</span>}
                    <span className={`name${filtered ? 'text-accent underline decoration-dotted' : ''}`}>{name}</span>
                    <span className="sort ml-1 text-accent">{sortMark}</span>
                    <span
                        className="absolute top-0 right-0 h-full w-1.5 cursor-col-resize resize"
                        data-resize={column}
                    />
                </div>,
            );
            x += width;
        }
    }

    return (
        <div
            ref={scrollerRef}
            className="absolute inset-0 overflow-auto outline-none"
            tabIndex={0}
            onScroll={() => redrawNow()}
            onKeyDown={(event) => controller.keyDown(event.nativeEvent)}
        >
            <div
                ref={layerRef}
                className="sticky top-0 left-0 z-1 overflow-hidden select-none"
                onMouseDown={(event) => controller.mouseDown(event.nativeEvent)}
            >
                {cells}
                {rowNumbers}
                {headers}
                {columns.length > 0 && (
                    <div
                        className="corner absolute top-0 left-0 z-3 h-7 border-r border-b border-grid-line bg-header"
                        style={{ width: rowNumberWidth }}
                    />
                )}
            </div>
            <div ref={spacerRef} className="pointer-events-none" />
        </div>
    );
}
