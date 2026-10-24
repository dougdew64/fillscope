// @ts-check
// The extension's logic, with no dependency on VS Code, so tests can drive it with fake sessions.
//
// It publishes, for every debug session, what its program is doing (starting, running, stopped,
// moving, exited, or debugging terminated) or that the session has ended, and at a stop the stack
// and the innermost frame's locals (docs/DEBUGGER-LOCKSTEP.md). A wrong file means an explanation
// of a program state that never existed, which is why:
//   - there is one entry per session: a lockstep run debugs two programs at once, and one
//     session's "running" must not overwrite the other's stop;
//   - "not fetched" (variables: null, with variablesError) is distinct from "none" (variables: []);
//   - resuming is published too, whether the debugger announces it (a "continued" event) or VS
//     Code asks for it (continue, next, step...), which debuggers are not expected to announce; a
//     request is published only once the debugger has accepted it, since a refused one leaves the
//     program stopped; a resume of only another thread leaves the stopped thread's stop;
//   - a stop whose details arrive after the program has moved on is dropped, not published, and
//     no more requests are made for it;
//   - nothing changes a session once it has ended, and nothing from a session's earlier
//     incarnation (before a restart that reused its id) changes the current one;
//   - a shortened stack or list of locals says so, and a count the debugger only estimates is not
//     published as exact;
//   - the file names its writer, and every write carries a sequence number (increasing within one
//     writer) and a timestamp, and each entry the ones of its last change, so a reader can tell
//     what is current and whether the writer is still running;
//   - the file is replaced atomically (written to a temporary file, then renamed).
'use strict';

const fs = require('node:fs');
const path = require('node:path');

const FORMAT_VERSION = 3;
const FRAME_LIMIT = 40;
const VARIABLE_LIMIT = 100;
const CHILD_LIMIT = 20;
// How many structured locals have their members read. Each costs a request, and requests at a
// stop queue ahead of VS Code's own, so the count is kept small.
const EXPANDED_LIMIT = 10;

// Requests from VS Code that resume the program. Debug adapters are not expected to send a
// "continued" event for these, so the request's success is the signal.
const RESUMING_REQUESTS = new Set([
    'continue', 'next', 'stepIn', 'stepOut', 'stepBack', 'reverseContinue', 'restart',
]);
// Requests that move the program to another place without running it freely. The protocol says
// the adapter answers and then sends a "stopped" event, so the stop published until then is out
// of date, but the program isn't running either.
const MOVING_REQUESTS = new Set(['goto', 'restartFrame']);
// Requests whose success means the program has been started (or attached to) and runs.
const STARTING_REQUESTS = new Set(['launch', 'attach', 'configurationDone']);

/** @typedef {import('@vscode/debugprotocol').DebugProtocol.ProtocolMessage} ProtocolMessage */
/** @typedef {import('@vscode/debugprotocol').DebugProtocol.Request} Request */
/** @typedef {import('@vscode/debugprotocol').DebugProtocol.Response} Response */
/** @typedef {import('@vscode/debugprotocol').DebugProtocol.ContinueResponse} ContinueResponse */
/** @typedef {import('@vscode/debugprotocol').DebugProtocol.Event} Event */
/** @typedef {import('@vscode/debugprotocol').DebugProtocol.StoppedEvent} StoppedEvent */
/** @typedef {import('@vscode/debugprotocol').DebugProtocol.ContinuedEvent} ContinuedEvent */
/** @typedef {import('@vscode/debugprotocol').DebugProtocol.ExitedEvent} ExitedEvent */

/**
 * What the publisher needs from a debug session; VS Code's DebugSession provides it.
 * @typedef {object} Session
 * @property {string} id
 * @property {string} name
 * @property {{ id: string }} [parentSession]   set for a session started by another
 * @property {(command: string, args?: any) => PromiseLike<any>} customRequest
 */

/** @typedef {{ id: string, name: string, parentSession?: { id: string } }} SessionIdentity */

/**
 * @typedef {object} PublishedVariable
 * @property {string} name
 * @property {string} value
 * @property {string | null} type
 * @property {PublishedVariable[]} [children]   members of a structured value (one level)
 * @property {boolean} [childrenTruncated]
 * @property {string} [childrenError]           present when the members were not read, saying why
 */

/**
 * @typedef {object} PublishedFrame
 * @property {string} name
 * @property {string | null} path
 * @property {number} line
 * @property {number} column
 */

/**
 * @typedef {object} StopDetails
 * @property {{ reason: string, description: string | null, text: string | null, threadId: number | null }} stop
 *           as the debugger reported it: `text` is short (an exception's name, say), `description` full
 * @property {number | null} framesThreadId    the thread whose stack this is (for a stop that named
 *                                             none, the first thread, as VS Code shows it)
 * @property {PublishedFrame[]} frames
 * @property {number | null} frameCount        the exact number of frames; null if not known
 * @property {boolean} framesTruncated
 * @property {string | null} framesError
 * @property {string | null} variablesScope
 * @property {PublishedVariable[] | null} variables   null: not fetched (see variablesError); []: none
 * @property {boolean} variablesTruncated
 * @property {string | null} variablesError
 */

// A session's entry. A union on `state`, so the type checker knows that only a stopped entry has
// stop details, and code that reads them must check the state first.
/** @typedef {{ name: string, parentId: string | null, seq: number, changedAtMs: number }} EntryBase */
/** @typedef {{ state: 'starting' }} Starting       the session exists; its program isn't running yet */
/** @typedef {{ state: 'running' }} Running */
/** @typedef {{ state: 'stopped' } & StopDetails} Stopped */
/** @typedef {{ state: 'moving' }} Moving           goto or restartFrame accepted; a stop follows */
/** @typedef {{ state: 'exited', exitCode: number | null }} Exited       the program ended; the session may restart it */
/** @typedef {{ state: 'terminated' }} Terminated   debugging ended; the program may still be running */
/** @typedef {{ state: 'ended', endReason: string | null }} Ended          the session is over; final */
/** @typedef {Starting | Running | Stopped | Moving | Exited | Terminated | Ended} StateFields */
/** @typedef {EntryBase & StateFields} SessionState */

/**
 * @typedef {object} Writer
 * @property {string} program
 * @property {number} pid
 * @property {number} startedAtMs
 * @property {boolean} running   false in the writer's last write, as VS Code shuts it down
 */

/**
 * @typedef {object} DebugStateFile
 * @property {number} version
 * @property {Writer} writer
 * @property {number} seq             increases with every write by one writer
 * @property {number} writtenAtMs
 * @property {Record<string, SessionState>} sessions   keyed by session id
 */

/** @param {unknown} error */
function messageOf(error) {
    return error instanceof Error ? error.message : String(error);
}

// Thrown, instead of making a request, once a stop's details are no longer wanted.
class StaleStop extends Error {}

/**
 * Reads the variables behind `reference`; with `expand`, also one level of members of the first
 * EXPANDED_LIMIT structured values.
 * @param {(command: string, args: any) => Promise<any>} ask
 * @param {number} reference
 * @param {boolean} expand
 * @param {number} limit
 * @returns {Promise<{ variables: PublishedVariable[], truncated: boolean }>}
 */
async function readVariables(ask, reference, expand, limit) {
    const reply = await ask('variables', { variablesReference: reference });
    /** @type {import('@vscode/debugprotocol').DebugProtocol.Variable[]} */
    const all = reply?.variables ?? [];
    const kept = all.slice(0, limit);
    /** @type {PublishedVariable[]} */
    const variables = [];
    let expanded = 0;
    for (const v of kept) {
        /** @type {PublishedVariable} */
        const published = { name: v.name, value: v.value, type: v.type ?? null };
        if (expand && v.variablesReference > 0) {
            if (expanded === EXPANDED_LIMIT) {
                published.childrenError = `not read: only the first ${EXPANDED_LIMIT} structured locals are expanded`;
            } else {
                expanded += 1;
                try {
                    const members = await readVariables(ask, v.variablesReference, false, CHILD_LIMIT);
                    published.children = members.variables;
                    published.childrenTruncated = members.truncated;
                } catch (error) {
                    // A stale stop's result is dropped anyway; rethrown so the rest isn't read
                    // into a result nobody will use.
                    if (error instanceof StaleStop) {
                        throw error;
                    }
                    published.childrenError = messageOf(error);
                }
            }
        }
        variables.push(published);
    }
    return { variables, truncated: all.length > kept.length };
}

/**
 * Whether a stackTrace reply holds the whole stack. The protocol says a reply with fewer frames
 * than were asked for marks the end of the stack, whatever `totalFrames` says (which is a hint and
 * may overstate). With as many as were asked for, more may exist, and how many isn't known.
 * @param {number} got     frames returned
 * @param {number} asked   frames requested
 */
function countFrames(got, asked) {
    const complete = got < asked;
    return { frameCount: complete ? got : null, framesTruncated: !complete };
}

/**
 * The scope whose variables are published as the innermost frame's locals: the one the debugger
 * marks as the locals (the protocol's presentationHint), else the one named Local or Locals, else
 * the first other one. Never a scope the debugger calls expensive (registers, say), which costs
 * many requests and says little.
 * @param {import('@vscode/debugprotocol').DebugProtocol.Scope[]} scopes
 */
function chooseScope(scopes) {
    const cheap = scopes.filter((s) => !s.expensive);
    return cheap.find((s) => s.presentationHint === 'locals')
        ?? cheap.find((s) => /^locals?$/i.test(s.name))
        ?? cheap[0];
}

/**
 * Asks the debugger for the stack and the innermost frame's locals at a stop. Once `isStale()`
 * says the stop is no longer wanted, no further requests are made; the result is then incomplete,
 * and the caller drops it. `onThread` is told the thread whose stack is read, as soon as it is
 * known.
 * @param {Session} session
 * @param {StoppedEvent['body'] | undefined} body
 * @param {() => boolean} [isStale]
 * @param {(threadId: number) => void} [onThread]
 * @returns {Promise<StopDetails>}
 */
async function describeStop(session, body, isStale = () => false, onThread = () => {}) {
    /** @param {string} command @param {any} args */
    const ask = async (command, args) => {
        if (isStale()) {
            throw new StaleStop('the program has moved on');
        }
        return session.customRequest(command, args);
    };
    /** @type {StopDetails} */
    const details = {
        stop: {
            reason: String(body?.reason ?? 'unknown'),
            description: body?.description ?? null,
            text: body?.text ?? null,
            threadId: typeof body?.threadId === 'number' ? body.threadId : null,
        },
        framesThreadId: null,
        frames: [],
        frameCount: null,
        framesTruncated: false,
        framesError: null,
        variablesScope: null,
        variables: null,
        variablesTruncated: false,
        variablesError: null,
    };

    let threadId = details.stop.threadId;
    if (threadId === null) {
        // The protocol lets a stop of every thread name none. The stack shown is then the first
        // thread's, as VS Code shows it, and framesThreadId says which that was.
        try {
            const reply = await ask('threads', {});
            const first = reply?.threads?.[0]?.id;
            threadId = typeof first === 'number' ? first : null;
        } catch (error) {
            details.framesError = `the stop named no thread, and the threads could not be listed: ${messageOf(error)}`;
            details.variablesError = 'no stack to read locals from';
            return details;
        }
        if (threadId === null) {
            details.framesError = 'the stop named no thread, and the debugger listed none';
            details.variablesError = 'no stack to read locals from';
            return details;
        }
    }
    details.framesThreadId = threadId;
    onThread(threadId);

    /** @type {import('@vscode/debugprotocol').DebugProtocol.StackFrame | undefined} */
    let top;
    try {
        // One more frame than is kept, so a longer stack is detected even if the debugger doesn't
        // report the total.
        const asked = FRAME_LIMIT + 1;
        const reply = await ask('stackTrace', { threadId, startFrame: 0, levels: asked });
        /** @type {import('@vscode/debugprotocol').DebugProtocol.StackFrame[]} */
        const got = reply?.stackFrames ?? [];
        details.frames = got.slice(0, FRAME_LIMIT).map((f) => ({
            name: f.name, path: f.source?.path ?? null, line: f.line, column: f.column,
        }));
        Object.assign(details, countFrames(got.length, asked));
        top = got[0];
    } catch (error) {
        details.framesError = messageOf(error);
        details.variablesError = 'no stack to read locals from';
        return details;
    }
    if (top === undefined) {
        details.variablesError = 'the stack is empty';
        return details;
    }

    try {
        const reply = await ask('scopes', { frameId: top.id });
        /** @type {import('@vscode/debugprotocol').DebugProtocol.Scope[]} */
        const scopes = reply?.scopes ?? [];
        const scope = chooseScope(scopes);
        if (scope === undefined) {
            details.variablesError = scopes.length === 0
                ? 'the innermost frame has no scopes'
                : `the innermost frame has only expensive scopes (${scopes.map((s) => s.name).join(', ')})`;
            return details;
        }
        details.variablesScope = scope.name;
        const read = await readVariables(ask, scope.variablesReference, true, VARIABLE_LIMIT);
        details.variables = read.variables;
        details.variablesTruncated = read.truncated;
    } catch (error) {
        details.variables = null;
        details.variablesError = messageOf(error);
    }
    return details;
}

/**
 * A request VS Code has sent that will change what the program does, and the debugger hasn't yet
 * answered.
 * @typedef {object} PendingRequest
 * @property {string} command
 * @property {number} generation               the session's generation when it was sent
 * @property {number | null} threadId
 * @property {boolean} singleThread            resumes only `threadId`
 */

/**
 * What the publisher knows of one session beyond its published entry.
 * @typedef {object} SessionRecord
 * @property {number} incarnation   which start of this session id this is (a restart reuses the id)
 * @property {number} generation    the counter's value at the session's last change
 * @property {Map<number, PendingRequest>} pending   keyed by the request's seq
 * @property {{ generation: number, threadId: number | null } | null} describing   the stop being read
 */

class DebugStatePublisher {
    /**
     * @param {(state: DebugStateFile) => void} write   called with the whole file after every change
     * @param {() => number} now
     * @param {number} pid
     */
    constructor(write, now = Date.now, pid = process.pid) {
        this.write = write;
        this.now = now;
        this.seq = 0;
        /** @type {Writer} */
        this.writer = { program: 'vscode-debug-state', pid, startedAtMs: now(), running: true };
        /** @type {Map<string, SessionState>} */
        this.sessions = new Map();
        // One counter for every session, which only increases. Each session records the value at
        // its last change, so a stop whose details arrive late can tell that the program has moved
        // on, even across a restart that reuses the session's id. Incarnations come from it too.
        this.counter = 0;
        /** @type {Map<string, SessionRecord>} */
        this.records = new Map();
    }

    /** As the writer shuts down: every session not yet ended is ended, and the writer is not running. */
    stop() {
        this.seq += 1;
        const now = this.now();
        for (const [id, entry] of this.sessions) {
            if (entry.state !== 'ended') {
                const record = this.records.get(id);
                if (record !== undefined) {
                    record.generation = ++this.counter;  // a stop still being read is now stale
                }
                this.sessions.set(id, {
                    name: entry.name, parentId: entry.parentId, state: 'ended',
                    endReason: 'VS Code closed the extension', seq: this.seq, changedAtMs: now,
                });
            }
        }
        this.writer.running = false;
        this.flush(now);
    }

    /**
     * A session has started: published as starting until its launch or attach succeeds. Sessions
     * that ended earlier are dropped. Returns this incarnation's token: pass it to the other
     * methods, so that anything still arriving from an earlier incarnation of the same session id
     * (its old debug adapter's last messages, after a restart) is ignored.
     * @param {SessionIdentity} session
     * @returns {number}
     */
    sessionStarted(session) {
        for (const [id, state] of this.sessions) {
            if (state.state === 'ended') {
                this.sessions.delete(id);
                this.records.delete(id);
            }
        }
        const incarnation = ++this.counter;
        this.records.set(session.id, { incarnation, generation: incarnation, pending: new Map(), describing: null });
        this.publish(session, { state: 'starting' });
        return incarnation;
    }

    /**
     * The session is over: published once, as ended, and nothing changes it afterwards.
     * @param {string} id
     * @param {string | null} [reason]   why, if it didn't end normally
     * @param {number} [incarnation]     from sessionStarted
     */
    sessionEnded(id, reason = null, incarnation = undefined) {
        const entry = this.sessions.get(id);
        if (entry === undefined || entry.state === 'ended' || !this.isCurrent(id, incarnation)) {
            return;
        }
        this.change({ id, name: entry.name, parentSession: entry.parentId === null ? undefined : { id: entry.parentId } },
            { state: 'ended', endReason: reason });
        this.records.get(id)?.pending.clear();
    }

    /**
     * A message from VS Code to the debugger. A request that will change what the program does is
     * remembered, and published when the debugger accepts it.
     * @param {Session} session
     * @param {ProtocolMessage} message
     * @param {number} [incarnation]   from sessionStarted
     */
    onClientMessage(session, message, incarnation = undefined) {
        const record = this.records.get(session.id);
        if (record === undefined || this.hasEnded(session.id) || !this.isCurrent(session.id, incarnation)
            || message?.type !== 'request') {
            return;
        }
        const request = /** @type {Request} */ (message);
        if (RESUMING_REQUESTS.has(request.command) || MOVING_REQUESTS.has(request.command)
            || STARTING_REQUESTS.has(request.command)) {
            record.pending.set(request.seq, {
                command: request.command,
                generation: record.generation,
                threadId: typeof request.arguments?.threadId === 'number' ? request.arguments.threadId : null,
                singleThread: request.arguments?.singleThread === true,
            });
        }
    }

    /**
     * A message from the debugger to VS Code: a stop is described and published; an accepted
     * request, or a "continued" event, says the program runs or moves; "exited" and "terminated"
     * say the program or the debugging ended. Resolves once any write it causes is done.
     * @param {Session} session
     * @param {ProtocolMessage} message
     * @param {number} [incarnation]   from sessionStarted
     */
    async onAdapterMessage(session, message, incarnation = undefined) {
        const record = this.records.get(session.id);
        if (record === undefined || this.hasEnded(session.id) || !this.isCurrent(session.id, incarnation)) {
            return;
        }
        if (message?.type === 'response') {
            this.onResponse(session, record, /** @type {Response} */ (message));
            return;
        }
        if (message?.type !== 'event') {
            return;
        }
        const event = /** @type {Event} */ (message);
        if (event.event === 'continued') {
            const body = /** @type {ContinuedEvent} */ (event).body;
            if (body?.allThreadsContinued === false && this.isAnotherThread(session.id, body.threadId)) {
                return;  // another thread resumed; the one whose stop is shown is still stopped
            }
            this.change(session, { state: 'running' });
        } else if (event.event === 'stopped') {
            const body = /** @type {StoppedEvent} */ (event).body;
            const generation = this.change(session, null);
            const describing = { generation, threadId: typeof body?.threadId === 'number' ? body.threadId : null };
            record.describing = describing;
            const isStale = () => record.generation !== generation;
            const details = await describeStop(session, body, isStale, (threadId) => { describing.threadId = threadId; });
            if (record.describing === describing) {
                record.describing = null;
            }
            if (isStale()) {
                return;  // the program moved on while the details were being read: this stop is stale
            }
            this.publish(session, { state: 'stopped', ...details });
        } else if (event.event === 'exited') {
            const exitCode = /** @type {ExitedEvent} */ (event).body?.exitCode;
            this.change(session, { state: 'exited', exitCode: typeof exitCode === 'number' ? exitCode : null });
        } else if (event.event === 'terminated') {
            // Debugging has ended, which the protocol says doesn't mean the program has exited
            // (a detached attach session, say). After "exited", that is already known.
            if (this.sessions.get(session.id)?.state !== 'exited') {
                this.change(session, { state: 'terminated' });
            }
        }
    }

    /**
     * @param {Session} session
     * @param {SessionRecord} record
     * @param {Response} response
     */
    onResponse(session, record, response) {
        const pending = record.pending.get(response.request_seq);
        if (pending === undefined) {
            return;  // a response to some other request
        }
        record.pending.delete(response.request_seq);
        if (!response.success) {
            return;  // refused: the program hasn't moved, so what is published stays true
        }
        if (record.generation !== pending.generation) {
            return;  // something newer (a stop, say) has already been seen
        }
        if (STARTING_REQUESTS.has(pending.command)) {
            if (this.sessions.get(session.id)?.state === 'starting') {
                this.change(session, { state: 'running' });
            }
            return;
        }
        if (MOVING_REQUESTS.has(pending.command)) {
            this.change(session, { state: 'moving' });
            return;
        }
        if (pending.singleThread && this.isAnotherThread(session.id, pending.threadId)) {
            return;
        }
        // The protocol's continue response says when not every thread resumed.
        const allContinued = /** @type {ContinueResponse} */ (response).body?.allThreadsContinued;
        if (pending.command === 'continue' && allContinued === false && this.isAnotherThread(session.id, pending.threadId)) {
            return;
        }
        this.change(session, { state: 'running' });
    }

    /**
     * Whether `threadId` names a thread other than the stopped one: the thread whose stop is being
     * read, or else the one whose stack is published.
     * @param {string} id
     * @param {number | null | undefined} threadId
     */
    isAnotherThread(id, threadId) {
        const record = this.records.get(id);
        let stopped = null;
        if (record?.describing && record.describing.generation === record.generation) {
            stopped = record.describing.threadId;
        } else {
            const entry = this.sessions.get(id);
            if (entry?.state === 'stopped') {
                stopped = entry.framesThreadId ?? entry.stop.threadId;
            }
        }
        return stopped !== null && typeof threadId === 'number' && threadId !== stopped;
    }

    /** @param {string} id */
    hasEnded(id) {
        return this.sessions.get(id)?.state === 'ended';
    }

    /**
     * Whether a call carrying `incarnation` is about the session's current incarnation. A call
     * without one (the tests', mostly) is taken as current.
     * @param {string} id
     * @param {number | undefined} incarnation
     */
    isCurrent(id, incarnation) {
        return incarnation === undefined || this.records.get(id)?.incarnation === incarnation;
    }

    /**
     * Records that the session changed (a new generation) and, given fields, publishes them.
     * Returns the new generation.
     * @param {SessionIdentity} session
     * @param {StateFields | null} fields
     */
    change(session, fields) {
        const generation = ++this.counter;
        const record = this.records.get(session.id);
        if (record !== undefined) {
            record.generation = generation;
        }
        if (fields !== null) {
            this.publish(session, fields);
        }
        return generation;
    }

    /**
     * @param {SessionIdentity} session
     * @param {StateFields} fields
     */
    publish(session, fields) {
        this.seq += 1;
        const now = this.now();
        this.sessions.set(session.id, {
            name: session.name, parentId: session.parentSession?.id ?? null, ...fields, seq: this.seq, changedAtMs: now,
        });
        this.flush(now);
    }

    /** @param {number} [now] */
    flush(now = this.now()) {
        this.write({
            version: FORMAT_VERSION, writer: { ...this.writer }, seq: this.seq, writtenAtMs: now,
            sessions: Object.fromEntries(this.sessions),
        });
    }
}

/**
 * The file-system calls writeAtomically makes, so a test can see them.
 * @typedef {{ mkdirSync: typeof fs.mkdirSync, writeFileSync: typeof fs.writeFileSync,
 *             renameSync: typeof fs.renameSync, rmSync: typeof fs.rmSync }} FileSystem
 */

/**
 * Replaces `file` with `state` atomically: written to a temporary file in the same directory, then
 * renamed over the old one, so a reader sees the old file or the new one, never part. On failure
 * the temporary file is removed and the error is thrown.
 * @param {string} file
 * @param {DebugStateFile} state
 * @param {FileSystem} [io]
 */
function writeAtomically(file, state, io = fs) {
    io.mkdirSync(path.dirname(file), { recursive: true });
    const temporary = `${file}.${process.pid}.tmp`;
    try {
        io.writeFileSync(temporary, JSON.stringify(state, null, 1) + '\n');
        io.renameSync(temporary, file);
    } catch (error) {
        io.rmSync(temporary, { force: true });
        throw error;
    }
}

/**
 * One line for the output channel about the latest write, e.g.
 * "#7 replay (self-stop): stopped at lockstep_anchor.cpp:104 in fillscope_request_anchor (3 frames, 2 locals)".
 * @param {DebugStateFile} state
 */
function summarize(state) {
    if (!state.writer.running) {
        const ended = Object.values(state.sessions).filter((s) => s.seq === state.seq).length;
        return `#${state.seq} the extension stopped${ended > 0 ? `; ${ended} session${ended === 1 ? '' : 's'} ended` : ''}`;
    }
    const latest = Object.values(state.sessions).find((s) => s.seq === state.seq);
    if (latest === undefined) {
        return `#${state.seq}`;
    }
    let line = `#${state.seq} ${latest.name}: ${latest.state}`;
    if (latest.state === 'stopped') {
        const top = latest.frames[0];
        if (top !== undefined) {
            line += ` at ${top.path === null ? '?' : path.basename(top.path)}:${top.line} in ${top.name}`;
        }
        const locals = latest.variables === null ? 'locals not read' : `${latest.variables.length} locals`;
        line += ` (${latest.frameCount ?? '?'} frames, ${locals})`;
    } else if (latest.state === 'exited' && latest.exitCode !== null) {
        line += ` (code ${latest.exitCode})`;
    } else if (latest.state === 'ended' && latest.endReason !== null) {
        line += ` (${latest.endReason})`;
    }
    return line;
}

module.exports = {
    DebugStatePublisher, describeStop, writeAtomically, summarize, messageOf,
    FRAME_LIMIT, VARIABLE_LIMIT, CHILD_LIMIT, EXPANDED_LIMIT, RESUMING_REQUESTS, MOVING_REQUESTS,
};
