// @ts-check
// The VS Code side (extension.js), run against a fake `vscode` module: which sessions it publishes,
// to which folder's file, how sessions end, and what it writes as VS Code starts and stops it.
//
// The fake keeps to what VS Code does (checked against VS Code 1.142's own code): a tracker's
// onExit is called only for a debug adapter run as a process, with its exit code (undefined when
// it was killed) and never a signal; onDidTerminateDebugSession fires for every session.
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
    /** @type {((session: any) => void)[]} */
    const terminateListeners = [];
    /** @type {string[]} */
    const lines = [];
    const vscode = {
        workspace: { workspaceFolders: folders.map((f) => ({ uri: { fsPath: f } })) },
        window: { createOutputChannel: () => ({ appendLine: (/** @type {string} */ l) => lines.push(l), dispose() {} }) },
        debug: {
            registerDebugAdapterTrackerFactory: (/** @type {string} */ _type, /** @type {any} */ f) => {
                factory = f;
                return { dispose() {} };
            },
            onDidTerminateDebugSession: (/** @type {(session: any) => void} */ listener) => {
                terminateListeners.push(listener);
                return { dispose() {} };
            },
        },
    };
    return {
        vscode,
        lines,
        factory: () => factory,
        terminate: (/** @type {any} */ session) => terminateListeners.forEach((listener) => listener(session)),
    };
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

/**
 * @param {string} id
 * @param {string | undefined} workspaceFolder
 * @param {{ parentId?: string, customRequest?: (command: string) => Promise<any> }} [options]
 */
function session(id, workspaceFolder, options = {}) {
    return {
        id, name: id, workspaceFolder: workspaceFolder === undefined ? undefined : { uri: { fsPath: workspaceFolder } },
        parentSession: options.parentId === undefined ? undefined : { id: options.parentId },
        customRequest: options.customRequest ?? (async () => ({})),
    };
}

/** Activates the extension in a workspace of `folders`; deactivated when the test ends. */
function activated(/** @type {import('node:test').TestContext} */ t, /** @type {string[]} */ folders) {
    const fake = fakeVscode(folders);
    const extension = loadExtension(fake.vscode);
    extension.activate({ subscriptions: [] });
    t.after(() => extension.deactivate());
    return { ...fake, extension };
}

/** Waits until `done()` is true, for the asynchronous work a message starts; fails after a second. */
async function eventually(/** @type {() => boolean} */ done) {
    const deadline = Date.now() + 1000;
    while (!done()) {
        assert.ok(Date.now() < deadline, 'timed out');
        await new Promise((resolve) => setTimeout(resolve, 1));
    }
}

test('a Fillscope session is published to its folder\'s file, replacing what an earlier VS Code left', (t) => {
    const dir = folder(t);
    fs.mkdirSync(path.join(dir, '.fillscope'));
    fs.writeFileSync(stateFile(dir), JSON.stringify({ seq: 99, sessions: { stale: { state: 'stopped' } } }));
    const { factory } = activated(t, [dir]);
    assert.ok(fs.readFileSync(stateFile(dir), 'utf8').includes('stale'), 'nothing written before a session');

    const tracker = factory().createDebugAdapterTracker(session('s1', dir));
    const state = readState(dir);
    assert.deepEqual(Object.keys(state.sessions), ['s1'], 'the earlier file\'s sessions are gone');
    assert.equal(state.sessions.s1.state, 'starting');
    assert.equal(state.writer.pid, process.pid);
    assert.equal(state.writer.running, true);

    tracker.onWillStopSession();
    assert.equal(readState(dir).sessions.s1.state, 'ended');
});

test('a session outside a Fillscope folder gets no tracker and writes nothing', (t) => {
    const other = folder(t, false);
    const { factory, lines } = activated(t, [other]);
    assert.equal(factory().createDebugAdapterTracker(session('s1', other)), undefined);
    assert.equal(factory().createDebugAdapterTracker(session('s2', undefined)), undefined);
    assert.equal(fs.existsSync(stateFile(other)), false);
    assert.deepEqual(lines, [], 'no output channel either');
});

test('in a workspace with a Fillscope folder, a session of another folder is still not published', (t) => {
    const dir = folder(t);
    const other = folder(t, false);
    const { factory } = activated(t, [dir, other]);
    assert.equal(factory().createDebugAdapterTracker(session('s1', other)), undefined);
    assert.equal(fs.existsSync(stateFile(dir)), false);
});

test('each Fillscope folder has its own file, including one added after activation', (t) => {
    const first = folder(t);
    const second = folder(t);
    const { factory, vscode } = activated(t, [first]);
    factory().createDebugAdapterTracker(session('a', first));
    vscode.workspace.workspaceFolders.push({ uri: { fsPath: second } });
    factory().createDebugAdapterTracker(session('b', second));

    assert.deepEqual(Object.keys(readState(first).sessions), ['a']);
    assert.deepEqual(Object.keys(readState(second).sessions), ['b']);
});

test('two workspace folders that reach one directory share one file', (t) => {
    const dir = folder(t);
    const link = `${dir}-link`;
    fs.symlinkSync(dir, link);
    t.after(() => fs.rmSync(link, { force: true }));
    const { factory } = activated(t, [dir, link]);
    factory().createDebugAdapterTracker(session('a', dir));
    factory().createDebugAdapterTracker(session('b', link));
    assert.deepEqual(Object.keys(readState(dir).sessions).sort(), ['a', 'b'], 'neither overwrote the other');
});

test('a session with no folder of its own goes to the first Fillscope folder', (t) => {
    const other = folder(t, false);
    const dir = folder(t);
    const { factory } = activated(t, [other, dir]);
    factory().createDebugAdapterTracker(session('s1', undefined));
    assert.deepEqual(Object.keys(readState(dir).sessions), ['s1']);
});

test('a child session names its parent', (t) => {
    const dir = folder(t);
    const { factory } = activated(t, [dir]);
    factory().createDebugAdapterTracker(session('parent', dir));
    factory().createDebugAdapterTracker(session('child', dir, { parentId: 'parent' }));
    assert.equal(readState(dir).sessions.child.parentId, 'parent');
});

test('an adapter process that exits says why only when it didn\'t end normally', (t) => {
    const dir = folder(t);
    const { factory } = activated(t, [dir]);
    // Each is checked before the next starts: a new session drops the ones already ended.
    factory().createDebugAdapterTracker(session('normal', dir)).onExit(0, undefined);
    assert.equal(readState(dir).sessions.normal.state, 'ended');
    assert.equal(readState(dir).sessions.normal.endReason, null, 'code 0 is a normal end, not a crash');
    factory().createDebugAdapterTracker(session('failed', dir)).onExit(1, undefined);
    assert.equal(readState(dir).sessions.failed.endReason, 'the debug adapter exited with code 1');
    factory().createDebugAdapterTracker(session('killed', dir)).onExit(undefined, undefined);
    assert.equal(readState(dir).sessions.killed.endReason, 'the debug adapter ended without an exit code');
});

test('a session VS Code ends any other way is ended too', (t) => {
    const dir = folder(t);
    const { factory, terminate } = activated(t, [dir]);
    const s = session('s1', dir);
    factory().createDebugAdapterTracker(s);
    terminate(s);
    assert.equal(readState(dir).sessions.s1.state, 'ended');
});

test('after a restart under the same id, the old adapter\'s late exit and messages change nothing', async (t) => {
    const dir = folder(t);
    const { factory } = activated(t, [dir]);
    const s = session('s1', dir, { customRequest: async () => ({ stackFrames: [] }) });
    const old = factory().createDebugAdapterTracker(s);
    old.onWillStopSession();
    const current = factory().createDebugAdapterTracker(s);  // VS Code's restart: a new tracker, the same id
    current.onDidSendMessage({ seq: 1, type: 'event', event: 'stopped', body: { reason: 'step', threadId: 1 } });
    await eventually(() => readState(dir).sessions.s1.state === 'stopped');
    old.onExit(1, undefined);
    old.onDidSendMessage({ seq: 2, type: 'event', event: 'continued', body: { threadId: 1 } });
    await new Promise((resolve) => setTimeout(resolve, 5));
    assert.equal(readState(dir).sessions.s1.state, 'stopped');
});

test('a failed write removes the old file, is logged, and the next write works again', (t) => {
    const dir = folder(t);
    const { factory, lines } = activated(t, [dir]);
    const tracker = factory().createDebugAdapterTracker(session('s1', dir));
    assert.ok(fs.existsSync(stateFile(dir)));
    // The temporary file's place taken by a directory: the write fails; the old file can be removed.
    const temporary = `${stateFile(dir)}.${process.pid}.tmp`;
    fs.mkdirSync(path.join(temporary, 'blocker'), { recursive: true });
    tracker.onWillStopSession();
    assert.equal(fs.existsSync(stateFile(dir)), false, 'no file, rather than a wrong one');
    assert.ok(lines.some((l) => l.startsWith('FAILED to write')), lines.join('\n'));

    fs.rmSync(temporary, { recursive: true });
    factory().createDebugAdapterTracker(session('s2', dir));
    assert.equal(readState(dir).sessions.s2.state, 'starting');
});

test('an adapter error is logged, and so is every write, a stop whose requests failed included', async (t) => {
    const dir = folder(t);
    const { factory, lines } = activated(t, [dir]);
    const s = session('s1', dir, { customRequest: async () => { throw new Error('the adapter refused'); } });
    const tracker = factory().createDebugAdapterTracker(s);
    tracker.onError(new Error('the pipe broke'));
    assert.ok(lines.includes('the debug adapter of s1 reported an error: the pipe broke'), lines.join('\n'));
    // A failed request is part of what is published (framesError), not a failure of the extension.
    tracker.onDidSendMessage({ seq: 1, type: 'event', event: 'stopped', body: { reason: 'step', threadId: 1 } });
    await eventually(() => readState(dir).sessions.s1.state === 'stopped');
    assert.equal(readState(dir).sessions.s1.framesError, 'the adapter refused');
    assert.ok(lines.some((l) => /^#\d+ s1: stopped/.test(l)), 'each write is logged');
});

test('when VS Code shuts the extension down, the file says so', (t) => {
    const dir = folder(t);
    const { factory, extension } = activated(t, [dir]);
    factory().createDebugAdapterTracker(session('s1', dir));
    extension.deactivate();

    const state = readState(dir);
    assert.equal(state.writer.running, false);
    assert.equal(state.sessions.s1.state, 'ended');
});

test('messages reach the publisher: a stop, then a resume the debugger accepts', async (t) => {
    const dir = folder(t);
    const { factory } = activated(t, [dir]);
    const s = session('s1', dir, { customRequest: async () => ({ stackFrames: [], totalFrames: 0 }) });
    const tracker = factory().createDebugAdapterTracker(s);
    tracker.onDidSendMessage({ seq: 1, type: 'event', event: 'stopped', body: { reason: 'step', threadId: 1 } });
    await eventually(() => readState(dir).sessions.s1.state === 'stopped');

    tracker.onWillReceiveMessage({ seq: 2, type: 'request', command: 'continue', arguments: { threadId: 1 } });
    tracker.onDidSendMessage({ seq: 3, type: 'response', request_seq: 2, command: 'continue', success: true });
    await eventually(() => readState(dir).sessions.s1.state === 'running');
});
