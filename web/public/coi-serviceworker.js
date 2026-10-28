// Makes the app cross-origin isolated where the server cannot send the
// headers for it, such as GitHub Pages: SharedArrayBuffer, which the core's
// threads need, is only there in an isolated page. Registered by
// src/isolation.ts; every response of the app's scope (the page, its
// scripts, the module and the threads' workers) gets the headers here.

self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', (event) => event.waitUntil(self.clients.claim()));

self.addEventListener('fetch', (event) => {
    const request = event.request;
    // Chromium rejects these when a service worker fetches them.
    if (request.cache === 'only-if-cached' && request.mode !== 'same-origin') return;
    event.respondWith(
        fetch(request).then((response) => {
            // Opaque responses (cross-origin, no-cors) cannot be changed.
            if (response.status === 0) return response;
            const headers = new Headers(response.headers);
            headers.set('Cross-Origin-Opener-Policy', 'same-origin');
            headers.set('Cross-Origin-Embedder-Policy', 'require-corp');
            return new Response(response.body, {
                status: response.status,
                statusText: response.statusText,
                headers,
            });
        }),
    );
});
