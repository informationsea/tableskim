// Cross-origin isolation, which SharedArrayBuffer and so the core's threads
// need, comes from two response headers. Hosts that cannot send them (GitHub
// Pages) get them from public/coi-serviceworker.js instead: registered here,
// it controls the page once the page is loaded again.

export type Isolation = 'isolated' | 'reloading' | 'unavailable';

// Set before the reload, so that a page still not isolated afterwards (the
// service worker could not help) does not reload over and over.
const RELOADED_KEY = 'tableskim-isolation-reload';

function storage(): Storage | null {
    try {
        return window.sessionStorage;
    } catch {
        return null; // blocked
    }
}

export async function ensureCrossOriginIsolation(): Promise<Isolation> {
    const session = storage();
    if (self.crossOriginIsolated) {
        session?.removeItem(RELOADED_KEY);
        return 'isolated';
    }
    // Service workers need a secure context (HTTPS, localhost); without one,
    // or after one reload in vain, the app explains what is missing.
    if (!window.isSecureContext || !('serviceWorker' in navigator) || !session || session.getItem(RELOADED_KEY)) {
        session?.removeItem(RELOADED_KEY);
        return 'unavailable';
    }
    try {
        await navigator.serviceWorker.register(`${import.meta.env.BASE_URL}coi-serviceworker.js`);
        await navigator.serviceWorker.ready;
    } catch {
        return 'unavailable';
    }
    session.setItem(RELOADED_KEY, '1');
    window.location.reload();
    return 'reloading';
}
