// What the VS Code extension's integration tests (vscode/tests/integration/)
// do in the webview, which they cannot reach otherwise. The extension marks
// the page with <meta name="tableskim-test"> in its test mode only.
//
// From the extension: {type: 'test', id, action: 'click' | 'text', selector}
// To the extension:   {type: 'testResult', id, text?, error?}

import { postToExtension } from './port';

const TIMEOUT = 30000;

type TestRequest = { type: 'test'; id: number; action: 'click' | 'text'; selector: string };

// Waits for an enabled element, as a user waits until they can click it.
async function enabledElement(selector: string) {
    const end = Date.now() + TIMEOUT;
    for (;;) {
        const element = document.querySelector<HTMLElement>(selector);
        if (element && !(element as HTMLButtonElement).disabled) return element;
        if (Date.now() > end) throw new Error(`no enabled ${selector}`);
        await new Promise((resolve) => setTimeout(resolve, 50));
    }
}

export function installTestHook() {
    window.addEventListener('message', (event: MessageEvent<TestRequest>) => {
        const request = event.data;
        if (request?.type !== 'test') return;
        const reply = (result: { text?: string; error?: string }) =>
            postToExtension({ type: 'testResult', id: request.id, ...result });
        if (request.action === 'text') reply({ text: document.querySelector(request.selector)?.textContent ?? '' });
        else
            enabledElement(request.selector).then(
                (element) => {
                    element.click();
                    reply({});
                },
                (error: Error) => reply({ error: error.message }),
            );
    });
}
