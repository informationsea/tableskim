import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import { App } from './App';
import { TableSkimClient } from './client';
import './index.css';
import { ensureCrossOriginIsolation } from './isolation';

const root = document.getElementById('root');
if (!root) throw new Error('no #root element');
const container = root;

// Without isolation the app explains what is missing; while the page is
// reloaded for it, the engine is not started.
void ensureCrossOriginIsolation().then((isolation) => {
    if (isolation === 'reloading') return;
    // One engine for the page (StrictMode would make two in a component).
    const client = new TableSkimClient(new URL(`${import.meta.env.BASE_URL}tableskim-wasm.js`, document.baseURI));
    createRoot(container).render(
        <StrictMode>
            <App client={client} />
        </StrictMode>,
    );
});
