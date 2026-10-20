// @ts-check
// The VS Code side: connects a publisher from debug_state.js to every debug session and writes
// .fillscope/debug-state.json. The logic, and most of its tests, are in debug_state.js.
//
// Extensions are installed per user, so this one is activated by any debug session in any
// workspace (package.json's activationEvents: "onDebug"), but writes only for a session whose
// workspace folder contains the extension's own source: a Fillscope folder. A session with no
// folder of its own is published to the first Fillscope folder open. Each Fillscope folder has its
// own file and publisher, created at the first session there, so a folder added to the workspace
// later is handled too; that session's write replaces whatever an earlier VS Code left in the
// file. One VS Code window per Fillscope folder: two would overwrite each other's file (the
// writer's process id in the file tells them apart).
//
// How VS Code says a session is over, and what this extension does with it:
//   - onWillStopSession, when VS Code is about to stop the session itself;
//   - vscode.debug.onDidTerminateDebugSession, for every session, however it ended;
//   - a tracker's onExit, only for a debug adapter VS Code runs as a process of its own (not
//     CodeLLDB, which runs inside VS Code), when that process exits. VS Code passes its exit code
//     (undefined if it was killed by a signal) and never a signal, so the reason given says only
//     what is known.
// The first of these to arrive ends the session; the others then change nothing.
'use strict';

const fs = require('node:fs');
const path = require('node:path');
const vscode = require('vscode');
const { DebugStatePublisher, writeAtomically, summarize, messageOf } = require('./debug_state.js');

const MARKER = path.join('tools', 'vscode-debug-state', 'package.json');
const STATE_FILE = path.join('.fillscope', 'debug-state.json');

/** @type {Map<string, DebugStatePublisher>} keyed by the Fillscope folder's real path */
const publishers = new Map();
/** @type {Map<string, { publisher: DebugStatePublisher, incarnation: number }>} keyed by session id */
const tracked = new Map();

/** @param {vscode.WorkspaceFolder} folder */
function isFillscope(folder) {
    return fs.existsSync(path.join(folder.uri.fsPath, MARKER));
}

/**
 * The Fillscope folder a session's state is published to, if any.
 * @param {vscode.DebugSession} session
 */
function folderFor(session) {
    if (session.workspaceFolder !== undefined) {
        return isFillscope(session.workspaceFolder) ? session.workspaceFolder : undefined;
    }
    return vscode.workspace.workspaceFolders?.find(isFillscope);
}

/**
 * Why a debug adapter's process ended, from what VS Code reports: an exit code of 0 is a normal
 * end (no reason); a missing code means it didn't exit by itself (killed, typically).
 * @param {number | undefined} code
 * @returns {string | null}
 */
function adapterExitReason(code) {
    if (code === 0) {
        return null;
    }
    return code === undefined ? 'the debug adapter ended without an exit code' : `the debug adapter exited with code ${code}`;
}

/** @param {vscode.ExtensionContext} context */
function activate(context) {
    /** @type {vscode.OutputChannel | undefined} */
    let output;
    // Created at the first Fillscope session, so other workspaces get no channel.
    const log = (/** @type {string} */ line) => {
        if (output === undefined) {
            output = vscode.window.createOutputChannel('Fillscope debug state');
            context.subscriptions.push(output);
        }
        output.appendLine(line);
    };

    /** @param {vscode.WorkspaceFolder} folder */
    const publisherFor = (folder) => {
        // The real path, so two workspace folders that reach one directory (one through a
        // symbolic link) share one publisher instead of overwriting each other's file.
        const key = fs.realpathSync(folder.uri.fsPath);
        const existing = publishers.get(key);
        if (existing !== undefined) {
            return existing;
        }
        const file = path.join(key, STATE_FILE);
        log(`writing ${file}`);
        const publisher = new DebugStatePublisher((state) => {
            try {
                writeAtomically(file, state);
                log(summarize(state));
            } catch (error) {
                // The old file no longer says what the debugger is doing. Removing it lets a
                // reader find no file rather than a wrong one; the next change writes again.
                fs.rmSync(file, { force: true });
                log(`FAILED to write ${file}: ${messageOf(error)}; removed the old file`);
            }
        });
        publishers.set(key, publisher);
        return publisher;
    };

    context.subscriptions.push(vscode.debug.registerDebugAdapterTrackerFactory('*', {
        createDebugAdapterTracker(session) {
            const folder = folderFor(session);
            if (folder === undefined) {
                return undefined;  // not a Fillscope session: nothing is written
            }
            const publisher = publisherFor(folder);
            // Every call below carries this tracker's incarnation, so the last calls of an
            // adapter replaced by a restart under the same session id change nothing.
            const incarnation = publisher.sessionStarted(session);
            tracked.set(session.id, { publisher, incarnation });
            return {
                onWillReceiveMessage: (message) => publisher.onClientMessage(session, message, incarnation),
                onDidSendMessage: (message) => {
                    publisher.onAdapterMessage(session, message, incarnation).catch((error) => {
                        log(`FAILED to describe a stop: ${messageOf(error)}`);
                    });
                },
                onWillStopSession: () => publisher.sessionEnded(session.id, null, incarnation),
                onError: (error) => log(`the debug adapter of ${session.name} reported an error: ${messageOf(error)}`),
                onExit: (code) => publisher.sessionEnded(session.id, adapterExitReason(code), incarnation),
            };
        },
    }));

    context.subscriptions.push(vscode.debug.onDidTerminateDebugSession((session) => {
        const entry = tracked.get(session.id);
        if (entry !== undefined) {
            entry.publisher.sessionEnded(session.id, null, entry.incarnation);
            tracked.delete(session.id);
        }
    }));
}

// VS Code is shutting the extension down (the window is closing or reloading): every publisher
// writes its last file, with each session ended and the writer not running.
function deactivate() {
    for (const publisher of publishers.values()) {
        publisher.stop();
    }
    publishers.clear();
    tracked.clear();
}

module.exports = { activate, deactivate };
