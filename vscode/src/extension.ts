// The TableSkim extension: the web app's frontend in a custom editor, with
// the WebAssembly core running in the extension host (editor.ts).

import * as vscode from 'vscode';
import { TABLE_VIEW_TYPE, TableSkimEditorProvider, TEXT_VIEW_TYPE } from './editor';

export function activate(context: vscode.ExtensionContext) {
    const provider = new TableSkimEditorProvider(context);
    // Kept while hidden: the webview would otherwise start over, reading the
    // file again and losing the view.
    const options = { webviewOptions: { retainContextWhenHidden: true } };
    context.subscriptions.push(
        vscode.window.registerCustomEditorProvider(TABLE_VIEW_TYPE, provider, options),
        vscode.window.registerCustomEditorProvider(TEXT_VIEW_TYPE, provider, options),
        vscode.commands.registerCommand('tableskim.open', async (uri?: vscode.Uri) => {
            const target = uri ?? vscode.window.activeTextEditor?.document.uri;
            if (target) await vscode.commands.executeCommand('vscode.openWith', target, TABLE_VIEW_TYPE);
        }),
    );
}

export function deactivate() {}
