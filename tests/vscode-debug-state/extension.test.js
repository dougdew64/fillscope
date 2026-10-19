// @ts-check
// The VS Code side (extension.js), run against a fake `vscode` module: which sessions it publishes,
// to which folder's file, and what it writes as VS Code starts and stops it.
'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const Module = require('node:module');
const os = require('node:os');
const path = require('node:path');
const { test } = require('node:test');

const EXTENSION = path.resolve(__dirname, '..', '..', 'tools', 'vscode-debug-state');

/**
 * A stand-in for the parts of VS Code's API the extension uses, recording what it is given.
 * @param {string[]} folders   the workspace's folders
 */
function fakeVscode(folders) {
    /** @type {any} */
    let factory;
    /** @type {string[]} */
    const lines = [];
    const vscode = {
        workspace: { workspaceFolders: folders.map((f) => ({ uri: { fsPath: f } })) },
        window: { createOutputChannel: () => ({ appendLine: (/** @type {string} */ l) => lines.push(l), dispose() {} }) },
        debug: { registerDebugAdapterTrackerFactory: (/** @type {string} */ _type, /** @type {any} */ f) => { factory = f; return { dispose() {} }; } },
    };
    return { vscode, lines, factory: () => factory };
}

/**
 * Loads a fresh copy of extension.js, with `require('vscode')` answered by `vscode`.
 * @param {object} vscode
 */
function loadExtension(vscode) {
    const file = path.join(EXTENSION, 'src', 'extension.js');
    delete require.cache[require.resolve(file)];
    const loader = /** @type {any} */ (Module);
    const original = loader._load;
    loader._load = function (/** @type {string} */ request, /** @type {any[]} */ ...rest) {
        return request === 'vscode' ? vscode : original.call(this, request, ...rest);
    };
    try {
        return require(file);
    } finally {
        loader._load = original;
    }
}

/** A workspace folder; a Fillscope folder if `fillscope`, holding the extension's own source. */
function folder(/** @type {import('node:test').TestContext} */ t, fillscope = true) {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'fillscope-extension-'));
    t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
    if (fillscope) {
        fs.mkdirSync(path.join(dir, 'tools', 'vscode-debug-state'), { recursive: true });
        fs.writeFileSync(path.join(dir, 'tools', 'vscode-debug-state', 'package.json'), '{}');
    }
    return dir;
}

/** @param {string} dir */
const stateFile = (dir) => path.join(dir, '.fillscope', 'debug-state.json');
/** @param {string} dir */
const readState = (dir) => JSON.parse(fs.readFileSync(stateFile(dir), 'utf8'));

/** @param {string} id @param {string | undefined} workspaceFolder */
function session(id, workspaceFolder) {
    return {
        id, name: id, workspaceFolder: workspaceFolder === undefined ? undefined : { uri: { fsPath: workspaceFolder } },
        customRequest: async () => ({}),
    };
}

test('a Fillscope session is published to its folder\'s file, which starts fresh', (t) => {
    const dir = folder(t);
    fs.mkdirSync(path.join(dir, '.fillscope'));
    fs.writeFileSync(stateFile(dir), JSON.stringify({ seq: 99, sessions: { stale: { state: 'stopped' } } }));
    const fake = fakeVscode([dir]);
    const extension = loadExtension(fake.vscode);
    extension.activate({ subscriptions: [] });
    assert.ok(fs.readFileSync(stateFile(dir), 'utf8').includes('stale'), 'nothing written before a session');

    const tracker = fake.factory().createDebugAdapterTracker(session('s1', dir));
    const state = readState(dir);
    assert.deepEqual(Object.keys(state.sessions), ['s1'], 'the earlier file\'s sessions are gone');
    assert.equal(state.sessions.s1.state, 'running');
    assert.equal(state.writer.pid, process.pid);
    assert.equal(state.writer.running, true);

    tracker.onWillStopSession();
    assert.equal(readState(dir).sessions.s1.state, 'ended');
    extension.deactivate();
});

test('a session outside a Fillscope folder gets no tracker and writes nothing', (t) => {
    const other = folder(t, false);
    const fake = fakeVscode([other]);
    const extension = loadExtension(fake.vscode);
    extension.activate({ subscriptions: [] });
    assert.equal(fake.factory().createDebugAdapterTracker(session('s1', other)), undefined);
    assert.equal(fake.factory().createDebugAdapterTracker(session('s2', undefined)), undefined);
    assert.equal(fs.existsSync(stateFile(other)), false);
    assert.deepEqual(fake.lines, [], 'no output channel either');
    extension.deactivate();
});

test('each Fillscope folder has its own file, including one added after activation', (t) => {
    const first = folder(t);
    const second = folder(t);
    const fake = fakeVscode([first]);
    const extension = loadExtension(fake.vscode);
    extension.activate({ subscriptions: [] });
    fake.factory().createDebugAdapterTracker(session('a', first));
    fake.vscode.workspace.workspaceFolders.push({ uri: { fsPath: second } });
    fake.factory().createDebugAdapterTracker(session('b', second));

    assert.deepEqual(Object.keys(readState(first).sessions), ['a']);
    assert.deepEqual(Object.keys(readState(second).sessions), ['b']);
    extension.deactivate();
});

test('a session with no folder of its own goes to the first Fillscope folder', (t) => {
    const other = folder(t, false);
    const dir = folder(t);
    const fake = fakeVscode([other, dir]);
    const extension = loadExtension(fake.vscode);
    extension.activate({ subscriptions: [] });
    fake.factory().createDebugAdapterTracker(session('s1', undefined));
    assert.deepEqual(Object.keys(readState(dir).sessions), ['s1']);
    extension.deactivate();
});

test('a crashed debug adapter ends its session, with the reason', (t) => {
    const dir = folder(t);
    const fake = fakeVscode([dir]);
    const extension = loadExtension(fake.vscode);
    extension.activate({ subscriptions: [] });
    const tracker = fake.factory().createDebugAdapterTracker(session('s1', dir));
    tracker.onExit(1, undefined);
    const entry = readState(dir).sessions.s1;
    assert.equal(entry.state, 'ended');
    assert.equal(entry.endReason, 'the debug adapter exited (code 1, signal none)');
    extension.deactivate();
});

test('when VS Code shuts the extension down, the file says so', (t) => {
    const dir = folder(t);
    const fake = fakeVscode([dir]);
    const extension = loadExtension(fake.vscode);
    extension.activate({ subscriptions: [] });
    fake.factory().createDebugAdapterTracker(session('s1', dir));
    extension.deactivate();

    const state = readState(dir);
    assert.equal(state.writer.running, false);
    assert.equal(state.sessions.s1.state, 'ended');
});

test('messages reach the publisher: a resume the debugger accepts is published', async (t) => {
    const dir = folder(t);
    const fake = fakeVscode([dir]);
    const extension = loadExtension(fake.vscode);
    extension.activate({ subscriptions: [] });
    const s = session('s1', dir);
    s.customRequest = async () => ({ stackFrames: [], totalFrames: 0 });
    const tracker = fake.factory().createDebugAdapterTracker(s);
    tracker.onDidSendMessage({ seq: 1, type: 'event', event: 'stopped', body: { reason: 'step', threadId: 1 } });
    await new Promise((resolve) => setTimeout(resolve, 20));
    assert.equal(readState(dir).sessions.s1.state, 'stopped');

    tracker.onWillReceiveMessage({ seq: 2, type: 'request', command: 'continue', arguments: { threadId: 1 } });
    tracker.onDidSendMessage({ seq: 3, type: 'response', request_seq: 2, command: 'continue', success: true });
    await new Promise((resolve) => setTimeout(resolve, 20));
    assert.equal(readState(dir).sessions.s1.state, 'running');
    extension.deactivate();
});
