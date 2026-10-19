// @ts-check
// The debug-state publisher, driven by fake debug sessions whose replies the tests script.
'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { test } = require('node:test');
const {
    DebugStatePublisher, writeAtomically, summarize, FRAME_LIMIT, VARIABLE_LIMIT, CHILD_LIMIT,
} = require('../../tools/vscode-debug-state/src/debug_state.js');

/** @typedef {import('../../tools/vscode-debug-state/src/debug_state.js').DebugStatePublisher} Publisher */

/**
 * A fake debug session. `replies` maps a request to its reply (or a function of its arguments);
 * a reply that is an Error makes the request fail.
 * @param {{ id?: string, name?: string, replies?: Record<string, any> }} [options]
 */
function fakeSession({ id = 's1', name = 'frame source', replies = {} } = {}) {
    /** @type {{ command: string, args: any }[]} */
    const requests = [];
    return {
        id,
        name,
        requests,
        /** @param {string} command @param {any} [args] */
        async customRequest(command, args) {
            requests.push({ command, args });
            const reply = typeof replies[command] === 'function' ? await replies[command](args) : replies[command];
            if (reply instanceof Error) {
                throw reply;
            }
            return reply;
        },
    };
}

/** A publisher whose writes are kept, with a clock the test controls. */
function recordingPublisher() {
    /** @type {import('../../tools/vscode-debug-state/src/debug_state.js').DebugStateFile[]} */
    const writes = [];
    let clock = 1000;
    const publisher = new DebugStatePublisher((state) => writes.push(structuredClone(state)), () => clock++);
    return { publisher, writes, last: () => writes[writes.length - 1] };
}

const STOPPED = { type: 'event', event: 'stopped', body: { reason: 'breakpoint', threadId: 1 } };

/** @param {number} count */
function frames(count) {
    return Array.from({ length: count }, (_, i) => ({
        id: 100 + i, name: i === 0 ? 'fillscope_frame_anchor' : `caller${i}`, line: 69 + i, column: 5,
        source: { path: '/repo/src/lockstep/anchor.cpp' },
    }));
}

/** A session stopped in the anchor, with one local and one structured local. */
function anchorSession(extra = {}) {
    return fakeSession({
        replies: {
            stackTrace: { stackFrames: frames(3), totalFrames: 3 },
            scopes: { scopes: [{ name: 'Locals', variablesReference: 7 }, { name: 'Registers', variablesReference: 8 }] },
            variables: (/** @type {any} */ args) => args.variablesReference === 7
                ? { variables: [
                    { name: 'frame_index', value: '12', type: 'uint64_t', variablesReference: 0 },
                    { name: 'frame', value: '{...}', type: 'Frame', variablesReference: 9 },
                ] }
                : { variables: [{ name: 'price', value: '10003', type: 'int64_t', variablesReference: 0 }] },
            ...extra,
        },
    });
}

test('a stop publishes the stack, the locals and the stop reason', async () => {
    const { publisher, last } = recordingPublisher();
    const session = anchorSession();
    publisher.sessionStarted(session);
    await publisher.onAdapterMessage(session, STOPPED);

    const entry = last().sessions.s1;
    assert.equal(entry.state, 'stopped');
    assert.equal(entry.stop?.reason, 'breakpoint');
    assert.deepEqual(entry.frames?.[0], { name: 'fillscope_frame_anchor', path: '/repo/src/lockstep/anchor.cpp', line: 69, column: 5 });
    assert.equal(entry.frameCount, 3);
    assert.equal(entry.framesTruncated, false);
    assert.equal(entry.variablesScope, 'Locals');
    assert.deepEqual(entry.variables?.map((v) => v.name), ['frame_index', 'frame']);
    assert.deepEqual(entry.variables?.[1].children, [{ name: 'price', value: '10003', type: 'int64_t' }]);
});

test('every write has a higher sequence number and a timestamp, and each entry those of its last change', async () => {
    const { publisher, writes } = recordingPublisher();
    const session = anchorSession();
    publisher.sessionStarted(session);
    await publisher.onAdapterMessage(session, STOPPED);
    publisher.onClientMessage(session, { type: 'request', command: 'continue' });

    assert.deepEqual(writes.map((w) => w.seq), [1, 2, 3]);
    for (const w of writes) {
        assert.ok(w.writtenAtMs > 0);
        assert.equal(w.version, 1);
    }
    const last = writes[2];
    assert.equal(last.sessions.s1.seq, 3);
    assert.equal(last.sessions.s1.changedAtMs, last.writtenAtMs);
});

test('locals that could not be read are null with a reason, never an empty list', async () => {
    const { publisher, last } = recordingPublisher();
    const session = anchorSession({ variables: new Error('the adapter refused') });
    publisher.sessionStarted(session);
    await publisher.onAdapterMessage(session, STOPPED);

    const entry = last().sessions.s1;
    assert.equal(entry.variables, null);
    assert.equal(entry.variablesError, 'the adapter refused');
});

test('a frame with no locals publishes an empty list, with no error', async () => {
    const { publisher, last } = recordingPublisher();
    const session = anchorSession({ variables: { variables: [] } });
    publisher.sessionStarted(session);
    await publisher.onAdapterMessage(session, STOPPED);

    const entry = last().sessions.s1;
    assert.deepEqual(entry.variables, []);
    assert.equal(entry.variablesError, null);
});

test('a stack that fails to load says why, and says the locals were not read', async () => {
    const { publisher, last } = recordingPublisher();
    const session = anchorSession({ stackTrace: new Error('no such thread') });
    publisher.sessionStarted(session);
    await publisher.onAdapterMessage(session, STOPPED);

    const entry = last().sessions.s1;
    assert.equal(entry.framesError, 'no such thread');
    assert.deepEqual(entry.frames, []);
    assert.equal(entry.variables, null);
    assert.ok(entry.variablesError);
});

test('a long stack is shortened and says so, with the true count when the debugger gives it', async () => {
    const { publisher, last } = recordingPublisher();
    const session = anchorSession({ stackTrace: { stackFrames: frames(FRAME_LIMIT + 1), totalFrames: 57 } });
    publisher.sessionStarted(session);
    await publisher.onAdapterMessage(session, STOPPED);

    const entry = last().sessions.s1;
    assert.equal(entry.frames?.length, FRAME_LIMIT);
    assert.equal(entry.framesTruncated, true);
    assert.equal(entry.frameCount, 57);
});

test('a long stack without a total says the count is unknown, not the number shown', async () => {
    const { publisher, last } = recordingPublisher();
    const session = anchorSession({ stackTrace: { stackFrames: frames(FRAME_LIMIT + 1) } });
    publisher.sessionStarted(session);
    await publisher.onAdapterMessage(session, STOPPED);

    const entry = last().sessions.s1;
    assert.equal(entry.framesTruncated, true);
    assert.equal(entry.frameCount, null);
});

test('many locals, or many members, are shortened and say so', async () => {
    const many = (/** @type {number} */ n) => Array.from({ length: n }, (_, i) => ({ name: `v${i}`, value: '0', variablesReference: 0 }));
    const { publisher, last } = recordingPublisher();
    const session = anchorSession({
        variables: (/** @type {any} */ args) => args.variablesReference === 7
            ? { variables: [{ name: 'big', value: '{...}', variablesReference: 9 }, ...many(VARIABLE_LIMIT)] }
            : { variables: many(CHILD_LIMIT + 5) },
    });
    publisher.sessionStarted(session);
    await publisher.onAdapterMessage(session, STOPPED);

    const entry = last().sessions.s1;
    assert.equal(entry.variables?.length, VARIABLE_LIMIT);
    assert.equal(entry.variablesTruncated, true);
    assert.equal(entry.variables?.[0].children?.length, CHILD_LIMIT);
    assert.equal(entry.variables?.[0].childrenTruncated, true);
});

test('a "continued" event publishes running and clears the stop', async () => {
    const { publisher, last } = recordingPublisher();
    const session = anchorSession();
    publisher.sessionStarted(session);
    await publisher.onAdapterMessage(session, STOPPED);
    await publisher.onAdapterMessage(session, { type: 'event', event: 'continued', body: { threadId: 1 } });

    const entry = last().sessions.s1;
    assert.equal(entry.state, 'running');
    assert.equal(entry.frames, undefined);
    assert.equal(entry.variables, undefined);
});

test('a resuming request from VS Code publishes running, since debuggers need not announce it', async () => {
    for (const command of ['continue', 'next', 'stepIn', 'stepOut']) {
        const { publisher, last } = recordingPublisher();
        const session = anchorSession();
        publisher.sessionStarted(session);
        await publisher.onAdapterMessage(session, STOPPED);
        publisher.onClientMessage(session, { type: 'request', command });
        assert.equal(last().sessions.s1.state, 'running', command);
    }
});

test('other requests from VS Code change nothing', async () => {
    const { publisher, writes } = recordingPublisher();
    const session = anchorSession();
    publisher.sessionStarted(session);
    await publisher.onAdapterMessage(session, STOPPED);
    const count = writes.length;
    publisher.onClientMessage(session, { type: 'request', command: 'evaluate' });
    publisher.onClientMessage(session, { type: 'request', command: 'stackTrace' });
    assert.equal(writes.length, count);
});

test('a stop whose details arrive after the program resumed is not published', async () => {
    const { publisher, last } = recordingPublisher();
    /** @type {(value: any) => void} */
    let releaseStack = () => {};
    const session = anchorSession({ stackTrace: () => new Promise((resolve) => { releaseStack = resolve; }) });
    publisher.sessionStarted(session);

    const stopping = publisher.onAdapterMessage(session, STOPPED);
    publisher.onClientMessage(session, { type: 'request', command: 'next' });  // resumed before the stack arrived
    releaseStack({ stackFrames: frames(3), totalFrames: 3 });
    await stopping;

    assert.equal(last().sessions.s1.state, 'running');
});

test('two sessions keep separate entries: one running does not hide the other stopped', async () => {
    const { publisher, last } = recordingPublisher();
    const source = anchorSession();
    const viewer = fakeSession({ id: 's2', name: 'viewer' });
    publisher.sessionStarted(viewer);
    publisher.sessionStarted(source);
    await publisher.onAdapterMessage(source, STOPPED);
    await publisher.onAdapterMessage(viewer, { type: 'event', event: 'continued', body: { threadId: 1 } });

    assert.equal(last().sessions.s1.state, 'stopped');
    assert.equal(last().sessions.s2.state, 'running');
});

test('a session that ends is published as ended, and dropped when a new session starts', () => {
    const { publisher, last } = recordingPublisher();
    publisher.sessionStarted({ id: 'old', name: 'frame source' });
    publisher.sessionEnded('old');
    assert.equal(last().sessions.old.state, 'ended');

    publisher.sessionStarted({ id: 'new', name: 'frame source' });
    assert.equal(last().sessions.old, undefined);
    assert.equal(last().sessions.new.state, 'running');
});

test('the file is replaced whole, and no temporary file is left behind', () => {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'fillscope-debug-state-'));
    const file = path.join(dir, '.fillscope', 'debug-state.json');
    const { publisher } = recordingPublisher();
    publisher.write = (state) => writeAtomically(file, state);
    publisher.sessionStarted({ id: 's1', name: 'frame source' });
    publisher.sessionEnded('s1');

    const written = JSON.parse(fs.readFileSync(file, 'utf8'));
    assert.equal(written.seq, 2);
    assert.equal(written.sessions.s1.state, 'ended');
    assert.deepEqual(fs.readdirSync(path.dirname(file)), ['debug-state.json']);
    fs.rmSync(dir, { recursive: true });
});

test('the output line names the session, the place and the counts', async () => {
    const { publisher, last } = recordingPublisher();
    const session = anchorSession();
    publisher.sessionStarted(session);
    await publisher.onAdapterMessage(session, STOPPED);
    assert.equal(summarize(last()), '#2 frame source: stopped at anchor.cpp:69 in fillscope_frame_anchor (3 frames, 2 locals)');
});
