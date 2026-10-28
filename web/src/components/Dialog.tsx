import { useEffect, useRef, type ReactNode } from 'react';
import { CheckpointBanner, type CheckpointBannerProps } from './Banners';

interface DialogProps {
    id: string;
    title: string;
    onClose: () => void;
    children: ReactNode;
    // Buttons at the bottom right.
    footer?: ReactNode;
    wide?: boolean;
    checkpoint?: CheckpointBannerProps;
}

/** A modal dialog (the browser's <dialog>: Escape closes it, focus stays in it). */
export function Dialog({ id, title, onClose, children, footer, wide, checkpoint }: DialogProps) {
    const dialog = useRef<HTMLDialogElement>(null);
    const close = useRef(onClose);
    useEffect(() => {
        close.current = onClose;
    });
    useEffect(() => {
        const element = dialog.current;
        if (!element) return;
        element.showModal();
        const cancel = (event: Event) => {
            event.preventDefault();
            close.current();
        };
        element.addEventListener('cancel', cancel);
        return () => {
            element.removeEventListener('cancel', cancel);
            element.close();
        };
    }, []);
    return (
        <dialog
            ref={dialog}
            id={id}
            aria-label={title}
            className={
                'm-auto flex max-h-[90vh] max-w-[95vw] flex-col rounded-md border border-line bg-surface p-0 text-fg ' +
                `shadow-xl backdrop:bg-black/30 ${wide ? 'w-[min(920px,95vw)]' : 'w-[min(480px,95vw)]'}`
            }
        >
            <header className="flex items-center border-b border-line px-3 py-2">
                <h2 className="flex-1 font-semibold">{title}</h2>
                <button
                    type="button"
                    className="dialog-close px-1 text-muted hover:text-fg"
                    aria-label="Close"
                    onClick={onClose}
                >
                    ✕
                </button>
            </header>
            {checkpoint?.checkpoint && <CheckpointBanner {...checkpoint} id={`${id}-checkpoint`} />}
            <div className="min-h-0 flex-1 overflow-auto p-3">{children}</div>
            {footer && <footer className="flex justify-end gap-2 border-t border-line px-3 py-2">{footer}</footer>}
        </dialog>
    );
}
