// @ts-check
// The VS Code side: connects a publisher from debug_state.js to every debug session and writes
// .fillscope/debug-state.json. The logic, and most of its tests, are in debug_state.js.
//
// Extensions are installed per user, so this one is activated by any debug session in any
// workspace (package.json's activationEvents: "onDebug"), but writes only for a session whose
// workspace folder contains the extension's own source: a Fillscope folder. A session with no
// folder of its own is published to the first Fillscope folder open. Each Fillscope folder has its
// own file and publisher, created at the first session there, so a folder added to the workspace
// later is handled too; that session's write replaces whatever an earlier VS Code left in the file. One VS Code window per Fillscope folder: two would overwrite each other's
// file (the writer's process id in the file tells them apart).
'use strict';

const fs = require('node:fs');
const path = require('node:path');
const vscode = require('vscode');
const { DebugStatePublisher, writeAtomically, summarize } = require('./debug_state.js');

const MARKER = path.join('tools', 'vscode-debug-state', 'package.json');
const STATE_FILE = path.join('.fillscope', 'debug-state.json');

/** @type {Map<string, DebugStatePublisher>} keyed by the Fillscope folder's path */
const publishers = new Map();

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
        const existing = publishers.get(folder.uri.fsPath);
        if (existing !== undefined) {
            return existing;
        }
        const file = path.join(folder.uri.fsPath, STATE_FILE);
        log(`writing ${file}`);
        const publisher = new DebugStatePublisher((state) => {
            try {
                writeAtomically(file, state);
                log(summarize(state));
            } catch (error) {
                log(`FAILED to write ${file}: ${messageOf(error)}`);
            }
        });
        publishers.set(folder.uri.fsPath, publisher);
        return publisher;
    };

    context.subscriptions.push(vscode.debug.registerDebugAdapterTrackerFactory('*', {
        createDebugAdapterTracker(session) {
            const folder = folderFor(session);
            if (folder === undefined) {
                return undefined; // not a Fillscope session: nothing is written
            }
            const publisher = publisherFor(folder);
            publisher.sessionStarted(session);
            return {
                onWillReceiveMessage: (message) => publisher.onClientMessage(session, message),
                onDidSendMessage: (message) => {
                    publisher.onAdapterMessage(session, message).catch((error) => {
                        log(`FAILED to describe a stop: ${messageOf(error)}`);
                    });
                },
                onWillStopSession: () => publisher.sessionEnded(session.id),
                // After a normal end the session is already ended and this changes nothing; after
                // the debug adapter crashes, it is the only word that the session is over.
                onExit: (code, signal) => publisher.sessionEnded(session.id,
                    `the debug adapter exited (code ${code ?? 'none'}, signal ${signal ?? 'none'})`),
            };
        },
    }));
}

// VS Code is shutting the extension down (the window is closing or reloading): every publisher
// writes its last file, with each session ended and the writer not running.
function deactivate() {
    for (const publisher of publishers.values()) {
        publisher.stop();
    }
    publishers.clear();
}

/** @param {unknown} error */
function messageOf(error) {
    return error instanceof Error ? error.message : String(error);
}

module.exports = { activate, deactivate };
