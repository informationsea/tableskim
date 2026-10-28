import { useRef, useState } from 'react';
import type { TableSkimClient } from '../client';
import { errorText } from '../hooks';
import type { SqlCompletion } from '../protocol';

interface SqlPanelProps {
    client: TableSkimClient;
    onResult: (sheet: string, sheets: string[], sql: string) => void;
    onClose: () => void;
}

const HISTORY_KEY = 'tableskim.sqlHistory';
const HISTORY_SIZE = 100;

// The history is the browser's, newest last; storage may be unavailable.
function loadHistory(): string[] {
    try {
        const stored: unknown = JSON.parse(localStorage.getItem(HISTORY_KEY) ?? '[]');
        return Array.isArray(stored) ? stored.filter((entry): entry is string => typeof entry === 'string') : [];
    } catch {
        return [];
    }
}

function addToHistory(sql: string): string[] {
    const history = [...loadHistory().filter((entry) => entry !== sql), sql].slice(-HISTORY_SIZE);
    try {
        localStorage.setItem(HISTORY_KEY, JSON.stringify(history));
    } catch {
        // Kept for this page only.
    }
    return history;
}

const encoder = new TextEncoder();

// The core counts in bytes of UTF-8; the editor in UTF-16 units.
function byteOffset(text: string, index: number): number {
    return encoder.encode(text.slice(0, index)).length;
}

function indexOfByte(text: string, bytes: number): number {
    let index = 0;
    let count = 0;
    for (const char of text) {
        if (count >= bytes) break;
        count += encoder.encode(char).length;
        index += char.length;
    }
    return index;
}

/** Runs SQL on the file (DuckDB, SQLite, Parquet, JSON); the result opens as a sheet. */
export function SqlPanel({ client, onResult, onClose }: SqlPanelProps) {
    const editor = useRef<HTMLTextAreaElement>(null);
    const [sql, setSql] = useState('');
    const [running, setRunning] = useState(false);
    const [status, setStatus] = useState('');
    const [failed, setFailed] = useState(false);
    const [candidates, setCandidates] = useState<{ completion: SqlCompletion; text: string } | null>(null);
    const [history, setHistory] = useState(loadHistory);
    const walk = useRef<{ index: number; draft: string } | null>(null);

    const run = async () => {
        const text = sql.trim();
        if (!text || running) return;
        setRunning(true);
        setCandidates(null);
        setStatus('Running…');
        setFailed(false);
        setHistory(addToHistory(text));
        walk.current = null;
        try {
            const reply = await client.call('sql', { sql: text });
            setStatus(reply.sheet ? `The result is in the sheet "${reply.sheet}".` : 'Done.');
            onResult(reply.sheet, reply.sheets, text);
        } catch (failure) {
            setStatus(errorText(failure));
            setFailed(true);
        } finally {
            setRunning(false);
        }
    };

    const replace = (text: string, begin: number, end: number, insert: string) => {
        const next = text.slice(0, begin) + insert + text.slice(end);
        setSql(next);
        requestAnimationFrame(() => {
            editor.current?.focus();
            editor.current?.setSelectionRange(begin + insert.length, begin + insert.length);
        });
    };

    const complete = async () => {
        const element = editor.current;
        if (!element) return;
        const text = element.value;
        const cursor = element.selectionStart;
        try {
            const completion = await client.call('completeSql', { sql: text, cursor: byteOffset(text, cursor) });
            if (completion.candidates.length === 0) {
                setCandidates(null);
                return;
            }
            const begin = indexOfByte(text, completion.begin);
            const end = indexOfByte(text, completion.end);
            if (completion.candidates.length === 1) {
                setCandidates(null);
                replace(text, begin, end, completion.candidates[0]);
                return;
            }
            if (completion.common.length > end - begin) replace(text, begin, end, completion.common);
            setCandidates({ completion, text: text.slice(0, begin) + completion.common + text.slice(end) });
        } catch (failure) {
            setStatus(errorText(failure));
            setFailed(true);
        }
    };

    const choose = (candidate: string) => {
        if (!candidates) return;
        const { completion, text } = candidates;
        const begin = indexOfByte(text, completion.begin);
        replace(text, begin, begin + completion.common.length, candidate);
        setCandidates(null);
    };

    // Ctrl+Up / Down (or Alt) walk the history, from the newest.
    const recall = (step: -1 | 1) => {
        const entries = loadHistory();
        if (entries.length === 0) return;
        const state = walk.current ?? { index: entries.length, draft: sql };
        const index = Math.max(0, Math.min(entries.length, state.index + step));
        walk.current = { ...state, index };
        setSql(index === entries.length ? state.draft : entries[index]);
    };

    const keyDown = (event: React.KeyboardEvent<HTMLTextAreaElement>) => {
        const modifier = event.ctrlKey || event.metaKey;
        if (event.key === 'Enter' && modifier) {
            event.preventDefault();
            void run();
        } else if (event.key === 'Tab' && !event.shiftKey && !modifier && !event.altKey) {
            event.preventDefault();
            void complete();
        } else if (event.key === ' ' && event.ctrlKey) {
            event.preventDefault();
            void complete();
        } else if ((event.key === 'ArrowUp' || event.key === 'ArrowDown') && (modifier || event.altKey)) {
            event.preventDefault();
            recall(event.key === 'ArrowUp' ? -1 : 1);
        } else if (event.key === 'Escape') setCandidates(null);
    };

    return (
        <section id="sql-panel" className="flex h-52 shrink-0 flex-col border-t border-line bg-chrome">
            <div className="flex items-center gap-2 px-2.5 py-1">
                <h2 className="font-semibold">SQL</h2>
                <button
                    type="button"
                    id="sql-run"
                    className="control"
                    disabled={running || !sql.trim()}
                    onClick={() => void run()}
                >
                    Run
                </button>
                <select
                    id="sql-history"
                    className="control max-w-72"
                    value=""
                    aria-label="History"
                    onChange={(event) => {
                        if (event.target.value) setSql(event.target.value);
                        walk.current = null;
                        editor.current?.focus();
                    }}
                >
                    <option value="">History…</option>
                    {[...history].reverse().map((entry, i) => (
                        <option key={i} value={entry}>
                            {entry.replace(/\s+/g, ' ').slice(0, 80)}
                        </option>
                    ))}
                </select>
                <span className="text-muted">Ctrl+Enter runs · Tab completes · Ctrl+↑/↓ history</span>
                <span className="flex-1" />
                <button
                    type="button"
                    className="px-1 text-muted hover:text-fg"
                    aria-label="Close SQL"
                    onClick={onClose}
                >
                    ✕
                </button>
            </div>
            <div className="relative min-h-0 flex-1 px-2.5">
                <textarea
                    ref={editor}
                    id="sql-editor"
                    className="control h-full w-full resize-none font-mono"
                    spellCheck={false}
                    placeholder="SELECT * FROM ..."
                    value={sql}
                    onChange={(event) => {
                        setSql(event.target.value);
                        setCandidates(null);
                        walk.current = null;
                    }}
                    onKeyDown={keyDown}
                />
                {candidates && (
                    <ul
                        id="sql-candidates"
                        className="absolute right-4 bottom-2 max-h-40 w-64 overflow-auto rounded border border-line bg-surface py-1 shadow-lg"
                    >
                        {candidates.completion.candidates.map((candidate) => (
                            <li key={candidate}>
                                <button
                                    type="button"
                                    className="w-full px-2 text-left font-mono hover:bg-active"
                                    onMouseDown={(event) => event.preventDefault()}
                                    onClick={() => choose(candidate)}
                                >
                                    {candidate}
                                </button>
                            </li>
                        ))}
                    </ul>
                )}
            </div>
            <p
                id="sql-status"
                className={`truncate px-2.5 py-1 ${failed ? 'text-danger' : 'text-muted'}`}
                title={status}
            >
                {status}
            </p>
        </section>
    );
}
