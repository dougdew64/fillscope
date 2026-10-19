// @ts-check
// The extension's manifest: when it is activated, and that what it names exists.
'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { test } = require('node:test');

const ROOT = path.resolve(__dirname, '..', '..');
const EXTENSION = path.join(ROOT, 'tools', 'vscode-debug-state');
const manifest = JSON.parse(fs.readFileSync(path.join(EXTENSION, 'package.json'), 'utf8'));

test('it is activated by debugging, the only time it has anything to write', () => {
    // Where it writes is decided per session, by extension.js (tested in extension.test.js).
    assert.deepEqual(manifest.activationEvents, ['onDebug']);
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
