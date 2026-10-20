// @ts-check
// The debug-state publisher, driven by fake debug sessions whose replies the tests script.
'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { test } = require('node:test');
const { Worker } = require('node:worker_threads');
const {
    DebugStatePublisher, writeAtomically, summarize, FRAME_LIMIT, VARIABLE_LIMIT, CHILD_LIMIT, EXPANDED_LIMIT,
    RESUMING_REQUESTS, MOVING_REQUESTS,
} = require('../../tools/vscode-debug-state/src/debug_state.js');

/** @typedef {import('../../tools/vscode-debug-state/src/debug_state.js').DebugStateFile} DebugStateFile */
/** @typedef {import('../../tools/vscode-debug-state/src/debug_state.js').SessionState} SessionState */
/** @typedef {import('../../tools/vscode-debug-state/src/debug_state.js').EntryBase} EntryBase */
/** @typedef {import('../../tools/vscode-debug-state/src/debug_state.js').Stopped} Stopped */

/**
 * A fake debug session. `replies` maps a request to its reply (or a function of its arguments);
 * a reply that is an Error makes the request fail.
 * @param {{ id?: string, name?: string, parentId?: string, replies?: Record<string, any> }} [options]
 */
function fakeSession({ id = 's1', name = 'frame source', parentId, replies = {} } = {}) {
    /** @type {{ command: string, args: any }[]} */
    const requests = [];
    return {
        id,
        name,
        parentSession: parentId === undefined ? undefined : { id: parentId },
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
    /** @type {DebugStateFile[]} */
    const writes = [];
    let clock = 1000;
    const publisher = new DebugStatePublisher((state) => writes.push(structuredClone(state)), () => clock++, 4321);
    return { publisher, writes, last: () => writes[writes.length - 1] };
}

// Protocol messages, each with its own sequence number as the protocol requires.
let nextSeq = 1;
/** @param {string} name @param {any} [body] */
const event = (name, body) => ({ seq: nextSeq++, type: 'event', event: name, body });
/** @param {string} command @param {any} [args] */
const request = (command, args) => ({ seq: nextSeq++, type: 'request', command, arguments: args });
/** @param {{ seq: number, command: string }} to @param {boolean} [success] @param {any} [body] */
const response = (to, success = true, body = undefined) => ({
    seq: nextSeq++, type: 'response', request_seq: to.seq, command: to.command, success, body,
});

const stopped = (threadId = 1) => event('stopped', { reason: 'breakpoint', threadId });

/**
 * VS Code asks for a resume and the debugger accepts it (or, with success false, refuses it).
 * @param {DebugStatePublisher} publisher
 * @param {ReturnType<typeof fakeSession>} session
 * @param {string} command
 * @param {any} [args]
 * @param {boolean} [success]
 */
async function resume(publisher, session, command, args = { threadId: 1 }, success = true) {
    const sent = request(command, args);
    publisher.onClientMessage(session, sent);
    await publisher.onAdapterMessage(session, response(sent, success));
}

/**
 * The entry, checked to be stopped, with its stop details.
 * @param {SessionState | undefined} entry
 * @returns {EntryBase & Stopped}
 */
function stoppedEntry(entry) {
    assert.equal(entry?.state, 'stopped');
    return /** @type {EntryBase & Stopped} */ (entry);
}

/** @param {number} count */
function frames(count) {
    return Array.from({ length: count }, (_, i) => ({
        id: 100 + i, name: i === 0 ? 'fillscope_frame_anchor' : `caller${i}`, line: 69 + i, column: 5,
        source: { path: '/repo/src/lockstep/anchor.cpp' },
    }));
}

/**
 * A session stopped in the anchor, with one local and one structured local.
 * @param {Record<string, any>} [extra]   replies that replace the defaults
 * @param {{ id?: string, name?: string, parentId?: string }} [identity]
 */
function anchorSession(extra = {}, identity = {}) {
    return fakeSession({
        ...identity,
        replies: {
            stackTrace: { stackFrames: frames(3), totalFrames: 3 },
            scopes: { scopes: [{ name: 'Locals', variablesReference: 7 }, { name: 'Registers', variablesReference: 8, expensive: true }] },
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

/** A reply that waits until the test releases it. */
function heldReply() {
    /** @type {(value: any) => void} */
    let release = () => {};
    const reply = () => new Promise((resolve) => { release = resolve; });
    return { reply, release: (/** @type {any} */ value) => release(value) };
}

/** A publisher with one started session stopped in the anchor. */
async function stoppedInTheAnchor(extra = {}) {
    const recorded = recordingPublisher();
    const session = anchorSession(extra);
    recorded.publisher.sessionStarted(session);
    await recorded.publisher.onAdapterMessage(session, stopped());
    return { ...recorded, session };
}

// ---- A stop's details ----

test('a stop publishes the stack, the locals and the stop reason', async () => {
    const { last } = await stoppedInTheAnchor();
    const entry = stoppedEntry(last().sessions.s1);
    assert.equal(entry.stop.reason, 'breakpoint');
    assert.equal(entry.stop.threadId, 1);
    assert.deepEqual(entry.frames[0], { name: 'fillscope_frame_anchor', path: '/repo/src/lockstep/anchor.cpp', line: 69, column: 5 });
    assert.equal(entry.frameCount, 3);
    assert.equal(entry.framesTruncated, false);
    assert.equal(entry.variablesScope, 'Locals');
    assert.deepEqual(entry.variables?.map((v) => v.name), ['frame_index', 'frame']);
    assert.deepEqual(entry.variables?.[1].children, [{ name: 'price', value: '10003', type: 'int64_t' }]);
    assert.equal(entry.parentId, null);
});

test('the file names its writer, and every write has a higher sequence number and a timestamp', async () => {
    const { publisher, writes, session } = await stoppedInTheAnchor();
    await resume(publisher, session, 'continue');

    assert.deepEqual(writes.map((w) => w.seq), [1, 2, 3]);
    for (const w of writes) {
        assert.ok(w.writtenAtMs > 0);
        assert.equal(w.version, 3);
        assert.deepEqual(w.writer, { program: 'vscode-debug-state', pid: 4321, startedAtMs: 1000, running: true });
    }
    const last = writes[2];
    assert.equal(last.sessions.s1.seq, 3);
    assert.equal(last.sessions.s1.changedAtMs, last.writtenAtMs);
});

test('locals that could not be read are null with a reason, never an empty list', async () => {
    const { last } = await stoppedInTheAnchor({ variables: new Error('the adapter refused') });
    const entry = stoppedEntry(last().sessions.s1);
    assert.equal(entry.variables, null);
    assert.equal(entry.variablesError, 'the adapter refused');
});

test('a frame with no locals publishes an empty list, with no error', async () => {
    const { last } = await stoppedInTheAnchor({ variables: { variables: [] } });
    const entry = stoppedEntry(last().sessions.s1);
    assert.deepEqual(entry.variables, []);
    assert.equal(entry.variablesError, null);
});

test('a stack that fails to load says why, and says the locals were not read', async () => {
    const { last } = await stoppedInTheAnchor({ stackTrace: new Error('no such thread') });
    const entry = stoppedEntry(last().sessions.s1);
    assert.equal(entry.framesError, 'no such thread');
    assert.deepEqual(entry.frames, []);
    assert.equal(entry.variables, null);
    assert.equal(entry.variablesError, 'no stack to read locals from');
});

test('an empty stack says there were no locals to read', async () => {
    const { last } = await stoppedInTheAnchor({ stackTrace: { stackFrames: [], totalFrames: 0 } });
    const entry = stoppedEntry(last().sessions.s1);
    assert.equal(entry.frameCount, 0);
    assert.equal(entry.variables, null);
    assert.equal(entry.variablesError, 'the stack is empty');
});

test('a long stack is shortened and says so, and its count is unknown even if the debugger estimates one', async () => {
    // The protocol calls totalFrames a hint that may overstate, so 57 can't be published as exact.
    const { last } = await stoppedInTheAnchor({ stackTrace: { stackFrames: frames(FRAME_LIMIT + 1), totalFrames: 57 } });
    const entry = stoppedEntry(last().sessions.s1);
    assert.equal(entry.frames.length, FRAME_LIMIT);
    assert.equal(entry.framesTruncated, true);
    assert.equal(entry.frameCount, null);
});

test('a stack returned whole has an exact count, even when the debugger\'s total says more', async () => {
    // The protocol: a reply with fewer frames than were asked for marks the end of the stack.
    let entry = stoppedEntry((await stoppedInTheAnchor({ stackTrace: { stackFrames: frames(5) } })).last().sessions.s1);
    assert.equal(entry.frameCount, 5);
    assert.equal(entry.framesTruncated, false);

    entry = stoppedEntry((await stoppedInTheAnchor({ stackTrace: { stackFrames: frames(5), totalFrames: 9 } })).last().sessions.s1);
    assert.equal(entry.frameCount, 5, 'the total is a hint that may overstate');
    assert.equal(entry.framesTruncated, false);
});

test('many locals, or many members, are shortened and say so', async () => {
    const many = (/** @type {number} */ n) => Array.from({ length: n }, (_, i) => ({ name: `v${i}`, value: '0', variablesReference: 0 }));
    const { last } = await stoppedInTheAnchor({
        variables: (/** @type {any} */ args) => args.variablesReference === 7
            ? { variables: [{ name: 'big', value: '{...}', variablesReference: 9 }, ...many(VARIABLE_LIMIT)] }
            : { variables: many(CHILD_LIMIT + 5) },
    });
    const entry = stoppedEntry(last().sessions.s1);
    assert.equal(entry.variables?.length, VARIABLE_LIMIT);
    assert.equal(entry.variablesTruncated, true);
    assert.equal(entry.variables?.[0].children?.length, CHILD_LIMIT);
    assert.equal(entry.variables?.[0].childrenTruncated, true);
});

test('only the first few structured locals are expanded, and the rest say so', async () => {
    const structured = Array.from({ length: EXPANDED_LIMIT + 3 }, (_, i) => ({ name: `s${i}`, value: '{...}', variablesReference: 20 + i }));
    const { last, session } = await stoppedInTheAnchor({
        variables: (/** @type {any} */ args) => args.variablesReference === 7
            ? { variables: structured }
            : { variables: [{ name: 'x', value: '1', variablesReference: 0 }] },
    });
    const entry = stoppedEntry(last().sessions.s1);
    assert.equal(entry.variables?.filter((v) => v.children !== undefined).length, EXPANDED_LIMIT);
    assert.match(entry.variables?.[EXPANDED_LIMIT].childrenError ?? '', /^not read: only the first/);
    // stackTrace, scopes, the locals, and one request per expanded local.
    assert.equal(session.requests.length, 3 + EXPANDED_LIMIT);
});

test('members that could not be read say why', async () => {
    const { last } = await stoppedInTheAnchor({
        variables: (/** @type {any} */ args) => args.variablesReference === 7
            ? { variables: [{ name: 'frame', value: '{...}', variablesReference: 9 }] }
            : new Error('members unavailable'),
    });
    const entry = stoppedEntry(last().sessions.s1);
    assert.equal(entry.variables?.[0].childrenError, 'members unavailable');
    assert.equal(entry.variables?.[0].children, undefined);
});

test('failed scopes leave the locals unread, with the reason', async () => {
    const { last } = await stoppedInTheAnchor({ scopes: new Error('no scopes here') });
    const entry = stoppedEntry(last().sessions.s1);
    assert.equal(entry.variables, null);
    assert.equal(entry.variablesError, 'no scopes here');
});

test('without a scope named for locals, the first inexpensive one is read, never an expensive one', async () => {
    let entry = stoppedEntry((await stoppedInTheAnchor({ scopes: { scopes: [
        { name: 'Registers', variablesReference: 8, expensive: true }, { name: 'Arguments', variablesReference: 7 },
    ] } })).last().sessions.s1);
    assert.equal(entry.variablesScope, 'Arguments');

    entry = stoppedEntry((await stoppedInTheAnchor({ scopes: { scopes: [
        { name: 'Registers', variablesReference: 8, expensive: true },
    ] } })).last().sessions.s1);
    assert.equal(entry.variables, null);
    assert.equal(entry.variablesError, 'the innermost frame has only expensive scopes (Registers)');
});

test('a stop that names no thread shows the first thread\'s stack', async () => {
    const recorded = recordingPublisher();
    const session = anchorSession({ threads: { threads: [{ id: 5, name: 'main' }, { id: 6, name: 'other' }] } });
    recorded.publisher.sessionStarted(session);
    await recorded.publisher.onAdapterMessage(session, event('stopped', { reason: 'pause', allThreadsStopped: true }));

    const entry = stoppedEntry(recorded.last().sessions.s1);
    assert.equal(entry.stop.threadId, null, 'the stop, as the debugger reported it');
    assert.equal(entry.frames.length, 3);
    assert.equal(session.requests.find((r) => r.command === 'stackTrace')?.args.threadId, 5);
});

test('a stop that names no thread, in a program with none listed, says so', async () => {
    for (const threads of [{ threads: [] }, new Error('no threads')]) {
        const recorded = recordingPublisher();
        const session = anchorSession({ threads });
        recorded.publisher.sessionStarted(session);
        await recorded.publisher.onAdapterMessage(session, event('stopped', { reason: 'pause' }));
        const entry = stoppedEntry(recorded.last().sessions.s1);
        assert.match(entry.framesError ?? '', /^the stop named no thread/);
        assert.equal(entry.variables, null);
    }
});

// ---- Running, and resuming ----

test('a "continued" event publishes running and clears the stop', async () => {
    const { publisher, last, session } = await stoppedInTheAnchor();
    await publisher.onAdapterMessage(session, event('continued', { threadId: 1 }));
    const entry = last().sessions.s1;
    assert.equal(entry.state, 'running');
    assert.equal('frames' in entry, false);
    assert.equal('variables' in entry, false);
});

test('another thread continuing alone leaves the published stop; the stopped thread continuing clears it', async () => {
    const { publisher, last, session } = await stoppedInTheAnchor();
    await publisher.onAdapterMessage(session, event('continued', { threadId: 2, allThreadsContinued: false }));
    assert.equal(last().sessions.s1.state, 'stopped');
    await publisher.onAdapterMessage(session, event('continued', { threadId: 1, allThreadsContinued: false }));
    assert.equal(last().sessions.s1.state, 'running');
});

test('every resuming request publishes running once the debugger accepts it, not when it is sent', async () => {
    assert.deepEqual([...RESUMING_REQUESTS].sort(),
        ['continue', 'next', 'restart', 'reverseContinue', 'stepBack', 'stepIn', 'stepOut']);
    for (const command of RESUMING_REQUESTS) {
        const { publisher, last, session } = await stoppedInTheAnchor();
        const sent = request(command, { threadId: 1 });
        publisher.onClientMessage(session, sent);
        assert.equal(last().sessions.s1.state, 'stopped', `${command}: not yet accepted`);
        await publisher.onAdapterMessage(session, response(sent));
        assert.equal(last().sessions.s1.state, 'running', command);
    }
});

test('a refused resuming request leaves the stop published', async () => {
    // stepBack, say, without reverse debugging: the program never moved.
    const { publisher, last, session } = await stoppedInTheAnchor();
    await resume(publisher, session, 'stepBack', { threadId: 1 }, false);
    const entry = stoppedEntry(last().sessions.s1);
    assert.equal(entry.frames.length, 3);
});

test('a resume accepted after a newer stop arrived does not hide that stop', async () => {
    const { publisher, last, session } = await stoppedInTheAnchor();
    const sent = request('next', { threadId: 1 });
    publisher.onClientMessage(session, sent);
    await publisher.onAdapterMessage(session, stopped());  // this debugger announces the stop first
    await publisher.onAdapterMessage(session, response(sent));
    assert.equal(last().sessions.s1.state, 'stopped');
});

test('stepping another thread alone leaves the published stop', async () => {
    const { publisher, last, session } = await stoppedInTheAnchor();
    await resume(publisher, session, 'next', { threadId: 2, singleThread: true });
    assert.equal(last().sessions.s1.state, 'stopped');
    await resume(publisher, session, 'next', { threadId: 1, singleThread: true });
    assert.equal(last().sessions.s1.state, 'running');
});

test('other requests, and responses to them, change nothing', async () => {
    const { publisher, writes, session } = await stoppedInTheAnchor();
    const count = writes.length;
    for (const command of ['evaluate', 'stackTrace', 'setBreakpoints']) {
        const sent = request(command, {});
        publisher.onClientMessage(session, sent);
        await publisher.onAdapterMessage(session, response(sent));
    }
    await publisher.onAdapterMessage(session, response({ seq: 99999, command: 'continue' }));  // never sent
    await publisher.onAdapterMessage(session, event('output', { output: 'hello' }));
    assert.equal(writes.length, count);
});

// ---- Stops that are out of date ----

test('a stop whose details arrive after the program resumed is not published, and asks nothing more', async () => {
    const { publisher, last } = recordingPublisher();
    const held = heldReply();
    const session = anchorSession({ stackTrace: held.reply });
    publisher.sessionStarted(session);

    const stopping = publisher.onAdapterMessage(session, stopped());
    await resume(publisher, session, 'next');  // resumed before the stack arrived
    held.release({ stackFrames: frames(3), totalFrames: 3 });
    await stopping;

    assert.equal(last().sessions.s1.state, 'running');
    assert.deepEqual(session.requests.map((r) => r.command), ['stackTrace'], 'no scopes or variables for a stale stop');
});

test('of two overlapping stops, only the later is published', async () => {
    const { publisher, writes, last } = recordingPublisher();
    const held = heldReply();
    let calls = 0;
    const session = anchorSession({
        stackTrace: () => (++calls === 1 ? held.reply() : { stackFrames: frames(2), totalFrames: 2 }),
    });
    publisher.sessionStarted(session);
    const first = publisher.onAdapterMessage(session, stopped());
    await publisher.onAdapterMessage(session, stopped());
    held.release({ stackFrames: frames(3), totalFrames: 3 });
    await first;

    assert.equal(stoppedEntry(last().sessions.s1).frameCount, 2);
    assert.equal(writes.filter((w) => w.sessions.s1?.state === 'stopped').length, 1);
});

test('a session that ends while a stop is being read stays ended', async () => {
    const { publisher, last } = recordingPublisher();
    const held = heldReply();
    const session = anchorSession({ stackTrace: held.reply });
    publisher.sessionStarted(session);
    const stopping = publisher.onAdapterMessage(session, stopped());
    publisher.sessionEnded('s1');
    held.release({ stackFrames: frames(3), totalFrames: 3 });
    await stopping;
    assert.equal(last().sessions.s1.state, 'ended');
});

test('a stop read before a restart that reuses the session id is not published after it', async () => {
    // VS Code emulates a restart by ending the session and starting one with the same id.
    const { publisher, last } = recordingPublisher();
    const held = heldReply();
    let calls = 0;
    const session = anchorSession({
        stackTrace: () => (++calls === 1 ? held.reply() : { stackFrames: frames(2), totalFrames: 2 }),
    });
    publisher.sessionStarted(session);
    const before = publisher.onAdapterMessage(session, stopped());
    publisher.sessionEnded('s1');
    publisher.sessionStarted(session);
    await publisher.onAdapterMessage(session, event('continued', { threadId: 1 }));
    held.release({ stackFrames: frames(3), totalFrames: 3 });
    await before;
    assert.equal(last().sessions.s1.state, 'running');
});

// ---- Sessions ending, and programs exiting ----

test('nothing changes a session after it has ended', async () => {
    const { publisher, writes, session } = await stoppedInTheAnchor();
    publisher.sessionEnded('s1');
    const count = writes.length;
    await publisher.onAdapterMessage(session, event('continued', { threadId: 1 }));
    await publisher.onAdapterMessage(session, stopped());
    publisher.onClientMessage(session, request('continue', { threadId: 1 }));
    publisher.sessionEnded('s1', 'again');
    assert.equal(writes.length, count);
    assert.equal(writes[count - 1].sessions.s1.state, 'ended');
});

test('a session that ends is published as ended, and dropped when a new session starts', () => {
    const { publisher, last } = recordingPublisher();
    publisher.sessionStarted({ id: 'old', name: 'frame source' });
    publisher.sessionEnded('old', 'the debug adapter exited (code 1, signal none)');
    const ended = last().sessions.old;
    assert.equal(ended.state, 'ended');
    assert.equal(ended.state === 'ended' && ended.endReason, 'the debug adapter exited (code 1, signal none)');

    publisher.sessionStarted({ id: 'new', name: 'frame source' });
    assert.equal(last().sessions.old, undefined);
    assert.equal(last().sessions.new.state, 'starting');
});

test('a program that exits is published as exited with its code, and can be restarted', async () => {
    const { publisher, last, session } = await stoppedInTheAnchor();
    await publisher.onAdapterMessage(session, event('exited', { exitCode: 3 }));
    await publisher.onAdapterMessage(session, event('terminated', {}));
    const entry = last().sessions.s1;
    assert.equal(entry.state, 'exited');
    assert.equal(entry.state === 'exited' && entry.exitCode, 3, '"terminated" after "exited" keeps the code');

    await resume(publisher, session, 'restart', {});
    assert.equal(last().sessions.s1.state, 'running');
});

test('"terminated" says debugging ended, not that the program exited', async () => {
    // The protocol: terminated "does not mean that the debuggee itself has exited" (a detached
    // attach session, say).
    const { publisher, last, session } = await stoppedInTheAnchor();
    await publisher.onAdapterMessage(session, event('terminated', {}));
    assert.equal(last().sessions.s1.state, 'terminated');
});

// ---- Several sessions ----

test('two sessions keep separate entries: one running does not hide the other stopped', async () => {
    const { publisher, last } = recordingPublisher();
    const source = anchorSession();
    const viewer = fakeSession({ id: 's2', name: 'viewer' });
    publisher.sessionStarted(viewer);
    publisher.sessionStarted(source);
    await publisher.onAdapterMessage(source, stopped());
    await publisher.onAdapterMessage(viewer, event('continued', { threadId: 1 }));

    assert.equal(last().sessions.s1.state, 'stopped');
    assert.equal(last().sessions.s2.state, 'running');
});

test('a child session names its parent', () => {
    const { publisher, last } = recordingPublisher();
    publisher.sessionStarted(fakeSession({ id: 'parent', name: 'launcher' }));
    publisher.sessionStarted(fakeSession({ id: 'child', name: 'program', parentId: 'parent' }));
    assert.equal(last().sessions.child.parentId, 'parent');
    assert.equal(last().sessions.parent.parentId, null);
});

// ---- The writer's start and end ----

test('stop ends every session and says the writer has stopped', async () => {
    const { publisher, writes } = recordingPublisher();
    const held = heldReply();
    const session = anchorSession({ stackTrace: held.reply });
    publisher.sessionStarted(session);
    publisher.sessionStarted(fakeSession({ id: 's2', name: 'viewer' }));
    publisher.sessionEnded('s2');
    const stopping = publisher.onAdapterMessage(session, stopped());
    publisher.stop();
    held.release({ stackFrames: frames(3), totalFrames: 3 });
    await stopping;

    const last = writes[writes.length - 1];
    assert.equal(last.writer.running, false);
    assert.equal(last.sessions.s1.state, 'ended', 'a stop being read when the writer stopped stays unpublished');
    assert.equal(last.sessions.s1.state === 'ended' && last.sessions.s1.endReason, 'VS Code closed the extension');
    assert.equal(last.sessions.s2.state === 'ended' && last.sessions.s2.endReason, null, 'an already-ended session is left as it was');
});

// ---- The file ----

/** A fresh directory, removed when the test ends. */
function scratch(/** @type {import('node:test').TestContext} */ t) {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'fillscope-debug-state-'));
    t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
    return dir;
}

test('the file is replaced whole, and no temporary file is left behind', (t) => {
    const file = path.join(scratch(t), '.fillscope', 'debug-state.json');
    const publisher = new DebugStatePublisher((state) => writeAtomically(file, state));
    publisher.sessionStarted({ id: 's1', name: 'frame source' });
    publisher.sessionEnded('s1');

    const written = JSON.parse(fs.readFileSync(file, 'utf8'));
    assert.equal(written.seq, 2);
    assert.equal(written.sessions.s1.state, 'ended');
    assert.deepEqual(fs.readdirSync(path.dirname(file)), ['debug-state.json']);
});

test('a failed write throws and leaves no temporary file', (t) => {
    const dir = scratch(t);
    const file = path.join(dir, 'debug-state.json');
    fs.mkdirSync(path.join(file, 'blocker'), { recursive: true });  // rename can't replace a directory
    const { publisher } = recordingPublisher();
    publisher.write = (state) => writeAtomically(file, state);
    assert.throws(() => publisher.sessionStarted({ id: 's1', name: 'frame source' }));
    assert.deepEqual(fs.readdirSync(dir), ['debug-state.json']);
});

test('the file is written to a temporary file in its directory, then renamed over the old one', (t) => {
    const file = path.join(scratch(t), 'debug-state.json');
    /** @type {string[]} */
    const calls = [];
    const io = {
        mkdirSync: /** @type {typeof fs.mkdirSync} */ ((...args) => fs.mkdirSync(...args)),
        writeFileSync: /** @type {typeof fs.writeFileSync} */ ((target, data) => { calls.push(`write ${target}`); fs.writeFileSync(target, data); }),
        renameSync: /** @type {typeof fs.renameSync} */ ((from, to) => { calls.push(`rename ${from} ${to}`); fs.renameSync(from, to); }),
        rmSync: /** @type {typeof fs.rmSync} */ ((...args) => fs.rmSync(...args)),
    };
    writeAtomically(file, {
        version: 3, writer: { program: 'test', pid: 1, startedAtMs: 0, running: true }, seq: 1, writtenAtMs: 0, sessions: {},
    }, io);
    const temporary = `${file}.${process.pid}.tmp`;
    assert.deepEqual(calls, [`write ${temporary}`, `rename ${temporary} ${file}`]);
    assert.equal(path.dirname(temporary), path.dirname(file), 'the same directory, so the rename is atomic');
});

test('a reader never sees part of a write', async (t) => {
    const file = path.join(scratch(t), 'debug-state.json');
    // Large enough that a write takes more than one system call, so a torn write could be seen.
    /** @type {DebugStateFile} */
    const state = {
        version: 3, writer: { program: 'test', pid: 1, startedAtMs: 0, running: true }, seq: 0, writtenAtMs: 0,
        sessions: Object.fromEntries(Array.from({ length: 300 }, (_, i) => [`s${i}`, {
            name: 'x'.repeat(200), parentId: null, state: /** @type {const} */ ('running'), seq: i, changedAtMs: 0,
        }])),
    };
    writeAtomically(file, state);
    // The reader runs on another thread, reading and parsing the file as fast as it can, and counts
    // the reads it makes between the writer's "start" and "stop": only those overlap the writes.
    // (The deterministic check of how the file is written is the test above; this one shows a real
    // reader on another thread is never given part of a file.)
    const reader = new Worker(`
        const { parentPort, workerData } = require('node:worker_threads');
        const fs = require('node:fs');
        let reads = 0, torn = 0, counting = false, done = false;
        parentPort.on('message', (m) => {
            if (m === 'start') { counting = true; parentPort.postMessage('started'); } else { done = true; }
        });
        (function loop() {
            for (let i = 0; i < 20; ++i) {
                try { JSON.parse(fs.readFileSync(workerData, 'utf8')); } catch { if (counting) ++torn; }
                if (counting) ++reads;
            }
            if (done) { parentPort.postMessage({ reads, torn }); } else { setImmediate(loop); }
        })();`, { eval: true, workerData: file });
    await new Promise((resolve) => { reader.once('message', resolve); reader.postMessage('start'); });
    for (let seq = 1; seq <= 300; ++seq) {
        writeAtomically(file, { ...state, seq });
        await new Promise((resolve) => setImmediate(resolve));
    }
    const result = await new Promise((resolve) => { reader.once('message', resolve); reader.postMessage('stop'); });
    await reader.terminate();
    assert.ok(result.reads > 0, 'reads were made while the file was being written');
    assert.equal(result.torn, 0);
});

// ---- The output channel's line ----

test('the output line names the session, the place and the counts', async () => {
    const { publisher, last, session } = await stoppedInTheAnchor();
    assert.equal(summarize(last()), '#2 frame source: stopped at anchor.cpp:69 in fillscope_frame_anchor (3 frames, 2 locals)');
    await publisher.onAdapterMessage(session, event('exited', { exitCode: 3 }));
    assert.equal(summarize(last()), '#3 frame source: exited (code 3)');
    publisher.sessionEnded('s1', 'the debug adapter exited with code 1');
    assert.equal(summarize(last()), '#4 frame source: ended (the debug adapter exited with code 1)');
    publisher.sessionStarted(fakeSession({ id: 's2', name: 'viewer' }));
    publisher.sessionStarted(fakeSession({ id: 's3', name: 'other' }));
    publisher.stop();
    assert.equal(summarize(last()), '#7 the extension stopped; 2 sessions ended');
});

// ---- Starting ----

test('a session is starting until its launch is accepted, then running', async () => {
    const { publisher, last } = recordingPublisher();
    const session = anchorSession();
    publisher.sessionStarted(session);
    assert.equal(last().sessions.s1.state, 'starting');
    const launch = request('launch', {});
    publisher.onClientMessage(session, launch);
    assert.equal(last().sessions.s1.state, 'starting');
    await publisher.onAdapterMessage(session, response(launch));
    assert.equal(last().sessions.s1.state, 'running');
});

test('a launch that fails leaves the session starting, never running', async () => {
    const { publisher, writes } = recordingPublisher();
    const session = anchorSession();
    publisher.sessionStarted(session);
    const launch = request('launch', {});
    publisher.onClientMessage(session, launch);
    await publisher.onAdapterMessage(session, response(launch, false));
    publisher.sessionEnded('s1');
    assert.deepEqual(writes.map((w) => w.sessions.s1.state), ['starting', 'ended']);
});

// ---- Moving without running ----

test('goto and restartFrame make the stop out of date without claiming the program runs', async () => {
    assert.deepEqual([...MOVING_REQUESTS].sort(), ['goto', 'restartFrame']);
    for (const command of MOVING_REQUESTS) {
        const { publisher, last, session } = await stoppedInTheAnchor();
        await resume(publisher, session, command, { threadId: 1 });
        assert.equal(last().sessions.s1.state, 'moving', command);
        await publisher.onAdapterMessage(session, stopped());  // the protocol: a stop follows
        assert.equal(last().sessions.s1.state, 'stopped', command);
    }
});

// ---- Threads while a stop is being read ----

test('a stop being read survives another thread resuming alone', async () => {
    for (const resumeOther of [
        async (/** @type {DebugStatePublisher} */ p, /** @type {any} */ s) => p.onAdapterMessage(s, event('continued', { threadId: 2, allThreadsContinued: false })),
        async (/** @type {DebugStatePublisher} */ p, /** @type {any} */ s) => resume(p, s, 'next', { threadId: 2, singleThread: true }),
    ]) {
        const { publisher, last } = recordingPublisher();
        const held = heldReply();
        const session = anchorSession({ stackTrace: held.reply });
        publisher.sessionStarted(session);
        const stopping = publisher.onAdapterMessage(session, stopped(1));
        await resumeOther(publisher, session);
        held.release({ stackFrames: frames(3) });
        await stopping;
        assert.equal(last().sessions.s1.state, 'stopped', 'thread 1 is still at its breakpoint');
    }
});

test('a continue the debugger says resumed only another thread leaves the stop', async () => {
    const { publisher, last, session } = await stoppedInTheAnchor();
    const sent = request('continue', { threadId: 2 });
    publisher.onClientMessage(session, sent);
    await publisher.onAdapterMessage(session, response(sent, true, { allThreadsContinued: false }));
    assert.equal(last().sessions.s1.state, 'stopped');

    const all = request('continue', { threadId: 1 });
    publisher.onClientMessage(session, all);
    await publisher.onAdapterMessage(session, response(all, true, { allThreadsContinued: false }));
    assert.equal(last().sessions.s1.state, 'running', 'the stopped thread itself resumed');
});

test('a stop that names no thread publishes whose stack it shows, and keeps it against another thread\'s resume', async () => {
    const recorded = recordingPublisher();
    const session = anchorSession({ threads: { threads: [{ id: 5, name: 'main' }] } });
    recorded.publisher.sessionStarted(session);
    await recorded.publisher.onAdapterMessage(session, event('stopped', { reason: 'pause', allThreadsStopped: true }));
    assert.equal(stoppedEntry(recorded.last().sessions.s1).framesThreadId, 5);
    await recorded.publisher.onAdapterMessage(session, event('continued', { threadId: 6, allThreadsContinued: false }));
    assert.equal(recorded.last().sessions.s1.state, 'stopped');
});

// ---- Incarnations ----

test('calls from a session\'s earlier incarnation change nothing', async () => {
    const { publisher, writes } = recordingPublisher();
    const session = anchorSession();
    const old = publisher.sessionStarted(session);
    publisher.sessionEnded('s1', null, old);
    const current = publisher.sessionStarted(session);  // a restart under the same id
    await publisher.onAdapterMessage(session, stopped(), current);
    const count = writes.length;
    publisher.sessionEnded('s1', 'the debug adapter exited with code 1', old);  // the old adapter's late exit
    await publisher.onAdapterMessage(session, event('continued', { threadId: 1 }), old);
    publisher.onClientMessage(session, request('continue', {}), old);
    assert.equal(writes.length, count);
    assert.equal(writes[count - 1].sessions.s1.state, 'stopped');
});

// ---- Choosing the locals ----

test('the locals are the scope the debugger marks as such, else the one named so, never an expensive one', async () => {
    /** @param {any[]} scopes */
    const chosen = async (scopes) => stoppedEntry((await stoppedInTheAnchor({ scopes: { scopes } })).last().sessions.s1);
    let entry = await chosen([{ name: 'Arguments', variablesReference: 8 }, { name: 'Frame', variablesReference: 7, presentationHint: 'locals' }]);
    assert.equal(entry.variablesScope, 'Frame');
    entry = await chosen([{ name: 'Arguments', variablesReference: 8 }, { name: 'Locals', variablesReference: 7 }]);
    assert.equal(entry.variablesScope, 'Locals', 'by name, though not first');
    entry = await chosen([{ name: 'Thread Locals', variablesReference: 8 }, { name: 'Local', variablesReference: 7 }]);
    assert.equal(entry.variablesScope, 'Local', 'the name exactly, not one containing it');
    entry = await chosen([{ name: 'Locals', variablesReference: 8, expensive: true }, { name: 'Arguments', variablesReference: 7 }]);
    assert.equal(entry.variablesScope, 'Arguments', 'never an expensive scope, whatever its name');
});

test('a stop\'s text and description are published as the debugger gave them', async () => {
    const recorded = recordingPublisher();
    const session = anchorSession();
    recorded.publisher.sessionStarted(session);
    await recorded.publisher.onAdapterMessage(session,
        event('stopped', { reason: 'exception', threadId: 1, text: 'std::bad_alloc', description: 'Exception thrown: std::bad_alloc' }));
    const entry = stoppedEntry(recorded.last().sessions.s1);
    assert.equal(entry.stop.text, 'std::bad_alloc');
    assert.equal(entry.stop.description, 'Exception thrown: std::bad_alloc');

    await recorded.publisher.onAdapterMessage(session, event('stopped', { reason: 'exception', threadId: 1, text: 'std::bad_alloc' }));
    const textOnly = stoppedEntry(recorded.last().sessions.s1);
    assert.equal(textOnly.stop.text, 'std::bad_alloc');
    assert.equal(textOnly.stop.description, null, 'a text is not a description');
});

test('a stop that goes stale while members are being read asks nothing more', async () => {
    const { publisher, last } = recordingPublisher();
    const held = heldReply();
    const session = anchorSession({
        variables: (/** @type {any} */ args) => args.variablesReference === 7
            ? { variables: [{ name: 'a', value: '{...}', variablesReference: 9 }, { name: 'b', value: '{...}', variablesReference: 10 }] }
            : held.reply(),
    });
    publisher.sessionStarted(session);
    const stopping = publisher.onAdapterMessage(session, stopped());
    await new Promise((resolve) => setImmediate(resolve));
    await new Promise((resolve) => setImmediate(resolve));
    await resume(publisher, session, 'continue');
    held.release({ variables: [] });
    await stopping;
    assert.equal(last().sessions.s1.state, 'running');
    assert.equal(session.requests.filter((r) => r.command === 'variables').length, 2, 'the locals and a\'s members; not b\'s');
});

test('every write by the writer has a higher sequence number, its last one too', async () => {
    const { publisher, writes } = recordingPublisher();
    publisher.sessionStarted(fakeSession());
    publisher.stop();
    assert.deepEqual(writes.map((w) => w.seq), [1, 2]);
});
