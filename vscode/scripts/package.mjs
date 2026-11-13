// Packs the extension (npm run build first) as tableskim-<version>-vscode.vsix,
// with the version of the project (CMakeLists.txt) rather than the one in
// package.json, which a version bump does not touch.
import { execFileSync } from 'node:child_process';
import { readFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const extensionDir = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const cmake = readFileSync(path.join(extensionDir, '..', 'CMakeLists.txt'), 'utf8');
const version = /^project\(table-skim VERSION ([0-9.]+)/m.exec(cmake)?.[1];
if (!version) {
    console.error('no project version in CMakeLists.txt');
    process.exit(1);
}
// The changes of a release are listed under its version before it is tagged.
const changelog = readFileSync(path.join(extensionDir, 'CHANGELOG.md'), 'utf8');
if (!changelog.includes(`\n## v${version}`) && !changelog.includes(`\n## ${version}`))
    console.warn(`CHANGELOG.md has no "## ${version}": rename "## Unreleased" before a release`);
// The README's images and links relative to it are taken from GitHub, as
// images/ stays out of the package.
const github = 'https://github.com/informationsea/tableskim';
const out = path.join(extensionDir, `tableskim-${version}-vscode.vsix`);
execFileSync(
    'npx',
    [
        'vsce',
        'package',
        version,
        '--no-git-tag-version',
        '--no-update-package-json',
        '--baseImagesUrl',
        `${github}/raw/HEAD/vscode`,
        '--baseContentUrl',
        `${github}/blob/HEAD/vscode`,
        '--out',
        out,
    ],
    { cwd: extensionDir, stdio: 'inherit', shell: process.platform === 'win32' },
);
