// @ts-check
// The extension's manifest: it activates only in the Fillscope workspace, and what it names exists.
'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { test } = require('node:test');

const ROOT = path.resolve(__dirname, '..', '..');
const EXTENSION = path.join(ROOT, 'tools', 'vscode-debug-state');
const manifest = JSON.parse(fs.readFileSync(path.join(EXTENSION, 'package.json'), 'utf8'));

test('it activates only in a workspace containing its own source', () => {
    assert.deepEqual(manifest.activationEvents, ['workspaceContains:tools/vscode-debug-state/package.json']);
    const marker = manifest.activationEvents[0].slice('workspaceContains:'.length);
    assert.ok(fs.existsSync(path.join(ROOT, marker)), 'the marker must exist in this repository');
});

test('the code it runs is the source itself, which exists', () => {
    assert.equal(manifest.main, './src/extension.js');
    assert.ok(fs.existsSync(path.join(EXTENSION, manifest.main)));
});

test('it has no run-time dependencies, only the development tools that check it', () => {
    assert.equal(manifest.dependencies, undefined);
    assert.deepEqual(Object.keys(manifest.devDependencies).sort(),
        ['@types/node', '@types/vscode', '@vscode/debugprotocol', 'typescript']);
});

test('extension.js checks for the same marker before writing anything', () => {
    const source = fs.readFileSync(path.join(EXTENSION, 'src', 'extension.js'), 'utf8');
    assert.match(source, /path\.join\('tools', 'vscode-debug-state', 'package\.json'\)/);
});
