// @ts-check
// The VS Code side: connects the publisher in debug_state.js to every debug session and writes
// .fillscope/debug-state.json. The logic, and its tests, are in debug_state.js.
'use strict';

const fs = require('node:fs');
const path = require('node:path');
const vscode = require('vscode');
const { DebugStatePublisher, writeAtomically, summarize } = require('./debug_state.js');

// Extensions are installed per user, so this one acts only in a workspace that contains its own
// source; anywhere else it does nothing. (package.json's activationEvents say the same.)
const MARKER = path.join('tools', 'vscode-debug-state', 'package.json');
const STATE_FILE = path.join('.fillscope', 'debug-state.json');

/** @param {vscode.ExtensionContext} context */
function activate(context) {
    const folder = vscode.workspace.workspaceFolders?.find((f) => fs.existsSync(path.join(f.uri.fsPath, MARKER)));
    if (folder === undefined) {
        return;
    }
    const file = path.join(folder.uri.fsPath, STATE_FILE);
    const output = vscode.window.createOutputChannel('Fillscope debug state');
    context.subscriptions.push(output);
    output.appendLine(`writing ${file}`);

    const publisher = new DebugStatePublisher((state) => {
        try {
            writeAtomically(file, state);
            output.appendLine(summarize(state));
        } catch (error) {
            output.appendLine(`FAILED to write ${file}: ${error}`);
        }
    });

    context.subscriptions.push(vscode.debug.registerDebugAdapterTrackerFactory('*', {
        createDebugAdapterTracker(session) {
            publisher.sessionStarted(session);
            return {
                onWillReceiveMessage: (message) => publisher.onClientMessage(session, message),
                onDidSendMessage: (message) => {
                    publisher.onAdapterMessage(session, message).catch((error) => {
                        output.appendLine(`FAILED to describe a stop: ${error}`);
                    });
                },
                onWillStopSession: () => publisher.sessionEnded(session.id),
            };
        },
    }));
}

function deactivate() {}

module.exports = { activate, deactivate };
