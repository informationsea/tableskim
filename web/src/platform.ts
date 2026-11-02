// What the app needs from where it runs: a browser page by default; the
// VS Code extension's webview (src/vscode/) sets its own before rendering.

export interface Platform {
    /** Whether the engine can start (in a browser: the page is cross-origin isolated). */
    engineAvailable: boolean;
    /** Hands a file the app made (an export, a chart) to the user to save. */
    saveFile: (blob: Blob, name: string) => void;
    /** Whether the page opens files of its own (Open…, Ctrl+O, dropped files). */
    opensFiles: boolean;
}

function downloadBlob(blob: Blob, name: string) {
    const url = URL.createObjectURL(blob);
    const link = document.createElement('a');
    link.href = url;
    link.download = name;
    document.body.append(link);
    link.click();
    link.remove();
    setTimeout(() => URL.revokeObjectURL(url), 10000);
}

let current: Platform = {
    // Threads need SharedArrayBuffer, which only a cross-origin isolated page has.
    engineAvailable: self.crossOriginIsolated,
    saveFile: downloadBlob,
    opensFiles: true,
};

export function platform(): Platform {
    return current;
}

export function setPlatform(next: Platform) {
    current = next;
}
