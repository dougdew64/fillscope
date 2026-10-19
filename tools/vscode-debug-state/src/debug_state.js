// @ts-check
// The extension's logic, with no dependency on VS Code, so tests can drive it with fake sessions.
//
// It publishes, for every debug session, whether the program is running, stopped or ended, and at a
// stop the stack and the innermost frame's locals (docs/DEBUGGER-LOCKSTEP.md). A wrong file means
// an explanation of a program state that never existed, which is why:
//   - there is one entry per session: a lockstep run debugs two programs at once, and one
//     session's "running" must not overwrite the other's stop;
//   - "not fetched" (variables: null, with variablesError) is distinct from "none" (variables: []);
//   - resuming is published too, whether the debugger announces it (a "continued" event) or VS
//     Code asks for it (continue, next, step...), which debuggers are not expected to announce;
//   - a stop whose details arrive after the program has resumed is dropped, not published;
//   - a shortened stack or list of locals says so;
//   - every write carries a sequence number and a timestamp, and each entry the ones of its last
//     change, so a reader can tell what is current;
//   - the file is replaced atomically (written to a temporary file, then renamed).
'use strict';

const fs = require('node:fs');
const path = require('node:path');

const FORMAT_VERSION = 1;
const FRAME_LIMIT = 40;
const VARIABLE_LIMIT = 100;
const CHILD_LIMIT = 20;

// Requests from VS Code that resume the program. Debug adapters are not expected to send a
// "continued" event for these, so the request itself is the signal.
const RESUMING_REQUESTS = new Set([
    'continue', 'next', 'stepIn', 'stepOut', 'stepBack', 'reverseContinue', 'goto', 'restartFrame',
]);

/**
 * What the publisher needs from a debug session; VS Code's DebugSession provides it.
 * @typedef {object} Session
 * @property {string} id
 * @property {string} name
 * @property {(command: string, args?: any) => PromiseLike<any>} customRequest
 */

/**
 * @typedef {object} PublishedVariable
 * @property {string} name
 * @property {string} value
 * @property {string | null} type
 * @property {PublishedVariable[]} [children]   members of a structured value (one level)
 * @property {boolean} [childrenTruncated]
 * @property {string} [childrenError]           present when the members could not be read
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
 * @property {number | null} frameCount        the true number of frames; null if unknown
 * @property {boolean} framesTruncated
 * @property {string | null} framesError
 * @property {string | null} variablesScope
 * @property {PublishedVariable[] | null} variables   null: not fetched (see variablesError); []: none
 * @property {boolean} variablesTruncated
 * @property {string | null} variablesError
 */

/**
 * @typedef {{ name: string, state: 'running' | 'stopped' | 'ended', seq: number, changedAtMs: number }
 *           & Partial<StopDetails>} SessionState
 */

/**
 * @typedef {object} DebugStateFile
 * @property {number} version
 * @property {number} seq             increases with every write
 * @property {number} writtenAtMs
 * @property {Record<string, SessionState>} sessions   keyed by session id
 */

/** @param {unknown} error */
function messageOf(error) {
    return error instanceof Error ? error.message : String(error);
}

/**
 * Reads the variables behind `reference`; with `expand`, also one level of members of each
 * structured value.
 * @param {Session} session
 * @param {number} reference
 * @param {boolean} expand
 * @param {number} limit
 * @returns {Promise<{ variables: PublishedVariable[], truncated: boolean }>}
 */
async function readVariables(session, reference, expand, limit) {
    const reply = await session.customRequest('variables', { variablesReference: reference });
    /** @type {import('@vscode/debugprotocol').DebugProtocol.Variable[]} */
    const all = reply?.variables ?? [];
    const kept = all.slice(0, limit);
    /** @type {PublishedVariable[]} */
    const variables = [];
    for (const v of kept) {
        /** @type {PublishedVariable} */
        const published = { name: v.name, value: v.value, type: v.type ?? null };
        if (expand && v.variablesReference > 0) {
            try {
                const members = await readVariables(session, v.variablesReference, false, CHILD_LIMIT);
                published.children = members.variables;
                published.childrenTruncated = members.truncated;
            } catch (error) {
                published.childrenError = messageOf(error);
            }
        }
        variables.push(published);
    }
    return { variables, truncated: all.length > kept.length };
}

/**
 * Asks the debugger for the stack and the innermost frame's locals at a stop.
 * @param {Session} session
 * @param {import('@vscode/debugprotocol').DebugProtocol.StoppedEvent['body'] | undefined} body
 * @returns {Promise<StopDetails>}
 */
async function describeStop(session, body) {
    const threadId = typeof body?.threadId === 'number' ? body.threadId : null;
    /** @type {StopDetails} */
    const details = {
        stop: { reason: String(body?.reason ?? 'unknown'), description: body?.description ?? body?.text ?? null, threadId },
        frames: [],
        frameCount: null,
        framesTruncated: false,
        framesError: null,
        variablesScope: null,
        variables: null,
        variablesTruncated: false,
        variablesError: null,
    };
    if (threadId === null) {
        details.framesError = 'the stop named no thread';
        details.variablesError = 'no stack to read locals from';
        return details;
    }

    /** @type {import('@vscode/debugprotocol').DebugProtocol.StackFrame | undefined} */
    let top;
    try {
        // One more frame than is kept, so a longer stack is detected even if the debugger doesn't
        // report the total.
        const reply = await session.customRequest('stackTrace', { threadId, startFrame: 0, levels: FRAME_LIMIT + 1 });
        /** @type {import('@vscode/debugprotocol').DebugProtocol.StackFrame[]} */
        const got = reply?.stackFrames ?? [];
        const total = typeof reply?.totalFrames === 'number' ? reply.totalFrames : null;
        details.frames = got.slice(0, FRAME_LIMIT).map((f) => ({
            name: f.name, path: f.source?.path ?? null, line: f.line, column: f.column,
        }));
        details.framesTruncated = got.length > FRAME_LIMIT || (total !== null && total > FRAME_LIMIT);
        details.frameCount = total !== null && total >= got.length ? total : details.framesTruncated ? null : got.length;
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
        const reply = await session.customRequest('scopes', { frameId: top.id });
        /** @type {import('@vscode/debugprotocol').DebugProtocol.Scope[]} */
        const scopes = reply?.scopes ?? [];
        const scope = scopes.find((s) => /local/i.test(s.name)) ?? scopes[0];
        if (scope === undefined) {
            details.variablesError = 'the innermost frame has no scopes';
            return details;
        }
        details.variablesScope = scope.name;
        const read = await readVariables(session, scope.variablesReference, true, VARIABLE_LIMIT);
        details.variables = read.variables;
        details.variablesTruncated = read.truncated;
    } catch (error) {
        details.variables = null;
        details.variablesError = messageOf(error);
    }
    return details;
}

class DebugStatePublisher {
    /**
     * @param {(state: DebugStateFile) => void} write   called with the whole file after every change
     * @param {() => number} now
     */
    constructor(write, now = Date.now) {
        this.write = write;
        this.now = now;
        this.seq = 0;
        /** @type {Map<string, SessionState>} */
        this.sessions = new Map();
        // Increases whenever a session's state changes, so a stop whose details arrive late can
        // tell that the program has moved on.
        /** @type {Map<string, number>} */
        this.generations = new Map();
    }

    /**
     * A session has started; it is running until it stops. Sessions that ended earlier are dropped.
     * @param {{ id: string, name: string }} session
     */
    sessionStarted(session) {
        for (const [id, state] of this.sessions) {
            if (state.state === 'ended') {
                this.sessions.delete(id);
                this.generations.delete(id);
            }
        }
        this.change(session.id, { name: session.name, state: 'running' });
    }

    /** @param {string} id */
    sessionEnded(id) {
        const state = this.sessions.get(id);
        if (state !== undefined) {
            this.change(id, { name: state.name, state: 'ended' });
        }
    }

    /**
     * A message from VS Code to the debugger: a request that resumes the program marks it running.
     * @param {{ id: string, name: string }} session
     * @param {any} message
     */
    onClientMessage(session, message) {
        if (message?.type === 'request' && RESUMING_REQUESTS.has(message.command)) {
            this.change(session.id, { name: session.name, state: 'running' });
        }
    }

    /**
     * A message from the debugger to VS Code: a stop is described and published; a "continued"
     * event marks the program running. Resolves once any write it causes is done.
     * @param {Session} session
     * @param {any} message
     */
    async onAdapterMessage(session, message) {
        if (message?.type !== 'event') {
            return;
        }
        if (message.event === 'continued') {
            this.change(session.id, { name: session.name, state: 'running' });
        } else if (message.event === 'stopped') {
            const generation = this.nextGeneration(session.id);
            const details = await describeStop(session, message.body);
            if (this.generations.get(session.id) !== generation) {
                return; // the program resumed while the details were being read: this stop is stale
            }
            this.publish(session.id, { name: session.name, state: 'stopped', ...details });
        }
    }

    /** @param {string} id */
    nextGeneration(id) {
        const generation = (this.generations.get(id) ?? 0) + 1;
        this.generations.set(id, generation);
        return generation;
    }

    /**
     * @param {string} id
     * @param {{ name: string, state: 'running' | 'ended' }} fields
     */
    change(id, fields) {
        this.nextGeneration(id);
        this.publish(id, fields);
    }

    /**
     * @param {string} id
     * @param {{ name: string, state: SessionState['state'] } & Partial<StopDetails>} fields
     */
    publish(id, fields) {
        this.seq += 1;
        const now = this.now();
        this.sessions.set(id, { ...fields, seq: this.seq, changedAtMs: now });
        this.write({ version: FORMAT_VERSION, seq: this.seq, writtenAtMs: now, sessions: Object.fromEntries(this.sessions) });
    }
}

/**
 * Replaces `file` with `state` atomically: a reader sees the old file or the new one, never part.
 * @param {string} file
 * @param {DebugStateFile} state
 */
function writeAtomically(file, state) {
    fs.mkdirSync(path.dirname(file), { recursive: true });
    const temporary = `${file}.${process.pid}.tmp`;
    fs.writeFileSync(temporary, JSON.stringify(state, null, 1) + '\n');
    fs.renameSync(temporary, file);
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
        const top = latest.frames?.[0];
        if (top !== undefined) {
            line += ` at ${top.path === null ? '?' : path.basename(top.path)}:${top.line} in ${top.name}`;
        }
        const locals = latest.variables === null || latest.variables === undefined ? 'locals not read' : `${latest.variables.length} locals`;
        line += ` (${latest.frameCount ?? '?'} frames, ${locals})`;
    }
    return line;
}

module.exports = { DebugStatePublisher, describeStop, writeAtomically, summarize, FRAME_LIMIT, VARIABLE_LIMIT, CHILD_LIMIT };
