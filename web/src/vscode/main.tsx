// The entry of the VS Code extension's webview (vscode.html): the app with
// the engine in the extension host, opening the editor's document.

import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import { App } from '../App';
import { TableSkimClient } from '../client';
import '../index.css';
import { setPlatform } from '../platform';
import { ExtensionPort, postToExtension } from './port';
import './theme.css';

const root = document.getElementById('root');
if (!root) throw new Error('no #root element');

setPlatform({
    // Node.js has SharedArrayBuffer without cross-origin isolation.
    engineAvailable: true,
    saveFile: (blob, name) =>
        void blob.arrayBuffer().then((data) => postToExtension({ type: 'save', name, data: new Uint8Array(data) })),
    pickFile: () => postToExtension({ type: 'pick' }),
    // A dropped File has no path the engine could open.
    opensDroppedFiles: false,
});

// The extension names its document; only that name crosses, as the
// extension opens the file itself.
const fileName = document.querySelector<HTMLMetaElement>('meta[name="tableskim-file"]')?.content ?? 'file';
const client = new TableSkimClient(new ExtensionPort());
createRoot(root).render(
    <StrictMode>
        <App client={client} initialFile={new File([], fileName)} />
    </StrictMode>,
);
