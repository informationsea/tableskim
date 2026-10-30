// ESLint for the web app: type-aware TypeScript rules and the React hooks
// rules for src/, the plain rules for the Node.js scripts and tests.
// Formatting is Prettier's (eslint-config-prettier turns the overlapping
// rules off).
import js from '@eslint/js';
import prettier from 'eslint-config-prettier';
import reactHooks from 'eslint-plugin-react-hooks';
import reactRefresh from 'eslint-plugin-react-refresh';
import globals from 'globals';
import tseslint from 'typescript-eslint';

export default tseslint.config(
    { ignores: ['dist/', 'node_modules/'] },
    js.configs.recommended,
    {
        files: ['src/**/*.{ts,tsx}', 'vite.config.ts', 'vite.vscode.config.ts'],
        extends: [tseslint.configs.recommendedTypeChecked, reactHooks.configs.flat.recommended],
        languageOptions: {
            globals: globals.browser,
            parserOptions: { projectService: true, tsconfigRootDir: import.meta.dirname },
        },
    },
    {
        files: ['src/**/*.tsx'],
        extends: [reactRefresh.configs.vite],
    },
    {
        files: ['**/*.{js,mjs,cjs}'],
        languageOptions: { globals: globals.node },
    },
    {
        files: ['**/*.cjs'],
        languageOptions: { sourceType: 'commonjs' },
    },
    {
        files: ['public/coi-serviceworker.js'],
        languageOptions: { globals: globals.serviceworker },
    },
    {
        // Playwright runs the callbacks given to page.evaluate() in the page.
        files: ['tests/**', 'probe/run-browser.cjs'],
        languageOptions: { globals: { ...globals.node, ...globals.browser } },
    },
    prettier,
);
