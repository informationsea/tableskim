import { useState } from 'react';
import type { TableSkimClient } from '../client';
import { errorText } from '../hooks';
import type { SheetInfo } from '../protocol';
import { Dialog } from './Dialog';

interface GoToDialogProps {
    client: TableSkimClient;
    info: SheetInfo;
    onGo: (row: number | null, column: number | null) => void;
    onClose: () => void;
}

/** Goes to a row ("120"), a column ("C", or a header name) or a cell ("C120"). */
export function GoToDialog({ client, info, onGo, onClose }: GoToDialogProps) {
    const [text, setText] = useState('');
    const [error, setError] = useState('');

    const go = async () => {
        try {
            const reply = await client.call('goTo', { sheet: info.sheet, text });
            if ('error' in reply) setError(reply.error);
            else {
                onGo(reply.row, reply.column);
                onClose();
            }
        } catch (failure) {
            setError(errorText(failure));
        }
    };

    return (
        <Dialog
            id="go-to-dialog"
            title="Go To"
            onClose={onClose}
            footer={
                <>
                    <button type="button" className="control" onClick={() => void go()}>
                        Go
                    </button>
                    <button type="button" className="control" onClick={onClose}>
                        Cancel
                    </button>
                </>
            }
        >
            <form
                onSubmit={(event) => {
                    event.preventDefault();
                    void go();
                }}
            >
                <label className="flex flex-col gap-1">
                    A row (120), a column (C{info.hasHeader ? ' or a header name' : ''}) or a cell (C120)
                    <input
                        id="go-to-text"
                        className="control"
                        autoFocus
                        value={text}
                        onChange={(event) => {
                            setText(event.target.value);
                            setError('');
                        }}
                    />
                </label>
            </form>
            {error && <p className="mt-2 text-danger">{error}</p>}
        </Dialog>
    );
}
