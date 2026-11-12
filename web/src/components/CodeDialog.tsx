import { useEffect, useState } from 'react';
import type { TableSkimClient } from '../client';
import { download } from '../charts/image';
import { errorText } from '../hooks';
import type { CodeAnalysis, CodeLanguage } from '../protocol';
import { Dialog } from './Dialog';

const LANGUAGES: { language: CodeLanguage; label: string }[] = [
    { language: 'r', label: 'R (tidyverse)' },
    { language: 'polars', label: 'Python (polars + plotnine)' },
    { language: 'pandas', label: 'Python (pandas + matplotlib)' },
];

// The language chosen last, for the next dialog.
const LANGUAGE_KEY = 'tableskim.codeLanguage';

function storedLanguage(): CodeLanguage {
    try {
        const stored = localStorage.getItem(LANGUAGE_KEY);
        const found = LANGUAGES.find((item) => item.language === stored);
        if (found) return found.language;
    } catch {
        // Storage may be unavailable (private windows, file:// pages).
    }
    return 'r';
}

function storeLanguage(language: CodeLanguage) {
    try {
        localStorage.setItem(LANGUAGE_KEY, language);
    } catch {
        // As above: the choice is then not remembered.
    }
}

interface CodeDialogProps {
    client: TableSkimClient;
    sheet: number;
    hidden: number[];
    analysis: CodeAnalysis;
    onClose: () => void;
}

/** R or Python code repeating the view, a chart of it, or the Group By that made the sheet. */
export function CodeDialog({ client, sheet, hidden, analysis, onClose }: CodeDialogProps) {
    const [language, setLanguage] = useState<CodeLanguage>(storedLanguage);
    const [code, setCode] = useState<{ code: string; fileName: string } | null>(null);
    const [error, setError] = useState('');
    const [status, setStatus] = useState('');
    const params = { sheet, language, hidden, ...analysis };
    // The analysis is a new object on every render of the owner.
    const request = JSON.stringify(params);

    useEffect(() => {
        let current = true;
        client.call('analysisCode', params).then(
            (reply) => {
                if (!current) return;
                setCode(reply);
                setError('');
            },
            (failure: unknown) => {
                if (!current) return;
                setCode(null);
                setError(errorText(failure));
            },
        );
        return () => {
            current = false;
        };
        // `params` changes with `request`.
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [client, request]);

    const choose = (next: CodeLanguage) => {
        storeLanguage(next);
        setLanguage(next);
        setStatus('');
    };
    const copy = async () => {
        if (!code) return;
        try {
            await navigator.clipboard.writeText(code.code);
            setStatus('Copied');
        } catch (failure) {
            setError(`Cannot copy the code: ${errorText(failure)}`);
        }
    };
    const save = async () => {
        if (!code) return;
        try {
            if (await download(new Blob([code.code], { type: 'text/plain' }), code.fileName))
                setStatus(`Saved ${code.fileName}`);
        } catch (failure) {
            setError(`Cannot save the code: ${errorText(failure)}`);
        }
    };

    return (
        <Dialog
            id="code-dialog"
            title="Analysis Code"
            wide
            onClose={onClose}
            footer={
                <>
                    <span id="code-status" className="mr-auto self-center text-muted" role="status">
                        {status}
                    </span>
                    <button
                        type="button"
                        id="code-copy"
                        className="control"
                        disabled={!code}
                        onClick={() => void copy()}
                    >
                        Copy
                    </button>
                    <button
                        type="button"
                        id="code-save"
                        className="control"
                        disabled={!code}
                        onClick={() => void save()}
                    >
                        Save…
                    </button>
                    <button type="button" className="control" onClick={onClose}>
                        Close
                    </button>
                </>
            }
        >
            <p className="mb-2 text-muted">
                Code that reads the file and repeats the filter and what is shown, as a starting point: the libraries
                read types and draw with colors of their own. Set <code>path</code> to where the file is.
            </p>
            <fieldset className="mb-2 flex flex-wrap gap-3">
                {LANGUAGES.map((item) => (
                    <label key={item.language} className="flex items-center gap-1">
                        <input
                            type="radio"
                            name="code-language"
                            value={item.language}
                            checked={language === item.language}
                            onChange={() => choose(item.language)}
                        />
                        {item.label}
                    </label>
                ))}
            </fieldset>
            {error && <p className="mb-2 text-danger">{error}</p>}
            {code && (
                <pre
                    id="code-text"
                    className="max-h-[60vh] overflow-auto rounded border border-line bg-chrome p-2 font-mono text-xs"
                >
                    {code.code}
                </pre>
            )}
        </Dialog>
    );
}
