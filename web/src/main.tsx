import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import { App } from './App';
import { TableSkimClient } from './client';
import './index.css';

// One engine for the page (StrictMode would make two in a component).
const client = new TableSkimClient(new URL(`${import.meta.env.BASE_URL}tableskim-wasm.js`, document.baseURI));

const root = document.getElementById('root');
if (!root) throw new Error('no #root element');
createRoot(root).render(
    <StrictMode>
        <App client={client} />
    </StrictMode>,
);
