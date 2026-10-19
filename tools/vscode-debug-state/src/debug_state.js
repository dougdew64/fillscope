// @ts-check
// The extension's logic, with no dependency on VS Code, so tests can drive it with fake sessions.
//
// It publishes, for every debug session, whether the program is running, stopped, exited or the
// session has ended, and at a stop the stack and the innermost frame's locals
// (docs/DEBUGGER-LOCKSTEP.md). A wrong file means an explanation of a program state that never
// existed, which is why:
//   - there is one entry per session: a lockstep run debugs two programs at once, and one
//     session's "running" must not overwrite the other's stop;
//   - "not fetched" (variables: null, with variablesError) is distinct from "none" (variables: []);
//   - resuming is published too, whether the debugger announces it (a "continued" event) or VS
//     Code asks for it (continue, next, step...), which debuggers are not expected to announce; a
//     request is published only once the debugger has accepted it, since a refused one leaves the
//     program stopped;
//   - a stop whose details arrive after the program has moved on is dropped, not published, and
//     no more requests are made for it;
//   - nothing changes a session once it has ended;
//   - a shortened stack or list of locals says so, and a count the debugger only estimates is not
//     published as exact;
//   - the file names its writer, and every write carries a sequence number (increasing within one
//     writer) and a timestamp, and each entry the ones of its last change, so a reader can tell
//     what is current and whether the writer is still running;
//   - the file is replaced atomically (written to a temporary file, then renamed).
'use strict';

const fs = require('node:fs');
const path = require('node:path');

const FORMAT_VERSION = 2;
const FRAME_LIMIT = 40;
const VARIABLE_LIMIT = 100;
const CHILD_LIMIT = 20;
// How many structured locals have their members read. Each costs a request, and requests at a
// stop queue ahead of VS Code's own, so the count is kept small.
const EXPANDED_LIMIT = 10;

// Requests from VS Code that resume the program. Debug adapters are not expected to send a
// "continued" event for these, so the request's success is the signal.
const RESUMING_REQUESTS = new Set([
    'continue', 'next', 'stepIn', 'stepOut', 'stepBack', 'reverseContinue', 'goto', 'restartFrame', 'restart',
]);

/** @typedef {import('@vscode/debugprotocol').DebugProtocol.ProtocolMessage} ProtocolMessage */
/** @typedef {import('@vscode/debugprotocol').DebugProtocol.Request} Request */
/** @typedef {import('@vscode/debugprotocol').DebugProtocol.Response} Response */
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
 * @property {{ reason: string, description: string | null, threadId: number | null }} stop
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
/** @typedef {{ state: 'running' }} Running */
/** @typedef {{ state: 'stopped' } & StopDetails} Stopped */
/** @typedef {{ state: 'exited', exitCode: number | null }} Exited       the program ended; the session may restart it */
/** @typedef {{ state: 'ended', endReason: string | null }} Ended          the session is over; final */
/** @typedef {Running | Stopped | Exited | Ended} StateFields */
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
 * Reads the stack and frame count from a stackTrace reply. The protocol calls `totalFrames` a
 * hint that may overstate, so a count is published only when it is known: when the debugger
 * returned fewer frames than were asked for, it returned them all.
 * @param {import('@vscode/debugprotocol').DebugProtocol.StackFrame[]} got
 * @param {number | null} total   the debugger's totalFrames, if it gave one
 * @param {number} asked
 */
function countFrames(got, total, asked) {
    const complete = got.length < asked && (total === null || total === got.length);
    return {
        frameCount: complete ? got.length : null,
        framesTruncated: !complete || got.length > FRAME_LIMIT,
    };
}

/**
 * Asks the debugger for the stack and the innermost frame's locals at a stop. Once `isStale()`
 * says the stop is no longer wanted, no further requests are made; the result is then incomplete,
 * and the caller drops it.
 * @param {Session} session
 * @param {StoppedEvent['body'] | undefined} body
 * @param {() => boolean} [isStale]
 * @returns {Promise<StopDetails>}
 */
async function describeStop(session, body, isStale = () => false) {
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
            description: body?.description ?? body?.text ?? null,
            threadId: typeof body?.threadId === 'number' ? body.threadId : null,
        },
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
        // thread's, as VS Code shows it.
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

    /** @type {import('@vscode/debugprotocol').DebugProtocol.StackFrame | undefined} */
    let top;
    try {
        // One more frame than is kept, so a longer stack is detected even if the debugger doesn't
        // report the total.
        const asked = FRAME_LIMIT + 1;
        const reply = await ask('stackTrace', { threadId, startFrame: 0, levels: asked });
        /** @type {import('@vscode/debugprotocol').DebugProtocol.StackFrame[]} */
        const got = reply?.stackFrames ?? [];
        const total = typeof reply?.totalFrames === 'number' ? reply.totalFrames : null;
        details.frames = got.slice(0, FRAME_LIMIT).map((f) => ({
            name: f.name, path: f.source?.path ?? null, line: f.line, column: f.column,
        }));
        Object.assign(details, countFrames(got, total, asked));
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
        // The locals by name; failing that, the first scope the debugger doesn't call expensive
        // (registers, say, which cost many requests and say little).
        const scope = scopes.find((s) => /local/i.test(s.name)) ?? scopes.find((s) => !s.expensive);
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
 * A resuming request VS Code has sent and the debugger hasn't yet answered.
 * @typedef {object} PendingResume
 * @property {number | undefined} generation   the session's generation when it was sent
 * @property {number | null} threadId
 * @property {boolean} singleThread            resumes only `threadId`
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
        // on, even across a restart that reuses the session's id.
        this.generation = 0;
        /** @type {Map<string, number>} */
        this.generations = new Map();
        /** @type {Map<string, PendingResume>} keyed by `${session id}#${request seq}` */
        this.pendingResumes = new Map();
    }

    /** As the writer shuts down: every session not yet ended is ended, and the writer is not running. */
    stop() {
        this.seq += 1;
        const now = this.now();
        for (const [id, entry] of this.sessions) {
            if (entry.state !== 'ended') {
                this.generation += 1;
                this.generations.set(id, this.generation); // a stop still being read is now stale
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
     * A session has started; it is running until it stops. Sessions that ended earlier are dropped.
     * @param {{ id: string, name: string, parentSession?: { id: string } }} session
     */
    sessionStarted(session) {
        for (const [id, state] of this.sessions) {
            if (state.state === 'ended') {
                this.sessions.delete(id);
                this.generations.delete(id);
            }
        }
        this.change(session, { state: 'running' });
    }

    /**
     * The session is over: published once, as ended, and nothing changes it afterwards.
     * @param {string} id
     * @param {string | null} [reason]   why, if it didn't end normally
     */
    sessionEnded(id, reason = null) {
        const entry = this.sessions.get(id);
        if (entry !== undefined && entry.state !== 'ended') {
            this.change({ id, name: entry.name, parentSession: entry.parentId === null ? undefined : { id: entry.parentId } },
                { state: 'ended', endReason: reason });
            for (const key of this.pendingResumes.keys()) {
                if (key.startsWith(`${id}#`)) {
                    this.pendingResumes.delete(key);
                }
            }
        }
    }

    /**
     * A message from VS Code to the debugger. A request that resumes the program is remembered,
     * and published when the debugger accepts it.
     * @param {Session} session
     * @param {ProtocolMessage} message
     */
    onClientMessage(session, message) {
        if (this.hasEnded(session.id) || message?.type !== 'request') {
            return;
        }
        const request = /** @type {Request} */ (message);
        if (RESUMING_REQUESTS.has(request.command)) {
            this.pendingResumes.set(`${session.id}#${request.seq}`, {
                generation: this.generations.get(session.id),
                threadId: typeof request.arguments?.threadId === 'number' ? request.arguments.threadId : null,
                singleThread: request.arguments?.singleThread === true,
            });
        }
    }

    /**
     * A message from the debugger to VS Code: a stop is described and published; an accepted
     * resuming request or a "continued" event marks the program running; "exited" and
     * "terminated" mark it exited. Resolves once any write it causes is done.
     * @param {Session} session
     * @param {ProtocolMessage} message
     */
    async onAdapterMessage(session, message) {
        if (this.hasEnded(session.id)) {
            return;
        }
        if (message?.type === 'response') {
            this.onResponse(session, /** @type {Response} */ (message));
            return;
        }
        if (message?.type !== 'event') {
            return;
        }
        const event = /** @type {Event} */ (message);
        if (event.event === 'continued') {
            const body = /** @type {ContinuedEvent} */ (event).body;
            if (body?.allThreadsContinued === false && this.isAnotherThread(session.id, body.threadId)) {
                return; // another thread resumed; the one whose stop is published is still stopped
            }
            this.change(session, { state: 'running' });
        } else if (event.event === 'stopped') {
            const generation = this.change(session, null);
            const isStale = () => this.generations.get(session.id) !== generation;
            const details = await describeStop(session, /** @type {StoppedEvent} */ (event).body, isStale);
            if (this.generations.get(session.id) !== generation) {
                return; // the program resumed while the details were being read: this stop is stale
            }
            this.publish(session, { state: 'stopped', ...details });
        } else if (event.event === 'exited') {
            const exitCode = /** @type {ExitedEvent} */ (event).body?.exitCode;
            this.change(session, { state: 'exited', exitCode: typeof exitCode === 'number' ? exitCode : null });
        } else if (event.event === 'terminated') {
            if (this.sessions.get(session.id)?.state !== 'exited') {
                this.change(session, { state: 'exited', exitCode: null }); // ended without saying how
            }
        }
    }

    /**
     * @param {Session} session
     * @param {Response} response
     */
    onResponse(session, response) {
        const key = `${session.id}#${response.request_seq}`;
        const pending = this.pendingResumes.get(key);
        if (pending === undefined) {
            return; // a response to some other request
        }
        this.pendingResumes.delete(key);
        if (!response.success) {
            return; // refused: the program hasn't moved, so its stop stays published
        }
        if (this.generations.get(session.id) !== pending.generation) {
            return; // something newer (a stop, say) has already been published
        }
        if (pending.singleThread && this.isAnotherThread(session.id, pending.threadId)) {
            return;
        }
        this.change(session, { state: 'running' });
    }

    /**
     * Whether `threadId` names a thread other than the one whose stop is published.
     * @param {string} id
     * @param {number | null | undefined} threadId
     */
    isAnotherThread(id, threadId) {
        const entry = this.sessions.get(id);
        return entry?.state === 'stopped' && entry.stop.threadId !== null && typeof threadId === 'number'
            && threadId !== entry.stop.threadId;
    }

    /** @param {string} id */
    hasEnded(id) {
        return this.sessions.get(id)?.state === 'ended';
    }

    /**
     * Records that the session changed (a new generation) and, given fields, publishes them.
     * Returns the new generation.
     * @param {{ id: string, name: string, parentSession?: { id: string } }} session
     * @param {StateFields | null} fields
     */
    change(session, fields) {
        this.generation += 1;
        this.generations.set(session.id, this.generation);
        if (fields !== null) {
            this.publish(session, fields);
        }
        return this.generation;
    }

    /**
     * @param {{ id: string, name: string, parentSession?: { id: string } }} session
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
 * Replaces `file` with `state` atomically: a reader sees the old file or the new one, never part.
 * On failure the temporary file is removed and the error is thrown.
 * @param {string} file
 * @param {DebugStateFile} state
 */
function writeAtomically(file, state) {
    fs.mkdirSync(path.dirname(file), { recursive: true });
    const temporary = `${file}.${process.pid}.tmp`;
    try {
        fs.writeFileSync(temporary, JSON.stringify(state, null, 1) + '\n');
        fs.renameSync(temporary, file);
    } catch (error) {
        fs.rmSync(temporary, { force: true });
        throw error;
    }
}

/**
 * One line for the output channel about the entry that changed last, e.g.
 * "#7 frame source: stopped at anchor.cpp:69 in fillscope_frame_anchor (3 frames, 2 locals)".
 * @param {DebugStateFile} state
 */
function summarize(state) {
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
    DebugStatePublisher, describeStop, writeAtomically, summarize,
    FRAME_LIMIT, VARIABLE_LIMIT, CHILD_LIMIT, EXPANDED_LIMIT, RESUMING_REQUESTS,
};
