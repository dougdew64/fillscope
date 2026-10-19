# Plan

**Purpose:** the next steps, in order, and what "done" means for each. **Status:** live plan; a
step is removed when it lands (git keeps it), and the file is deleted when it's empty.
**Read when:** choosing what to work on next.

Decisions behind the steps are in the [charter](CHARTER.md); the lockstep design and its
experiment results are in [debugger-lockstep.md](debugger-lockstep.md). In parallel, the owner
reads the trading domain; the stage-by-stage split of who writes what (charter §4) waits on that.

## 1. The project skeleton
A top-level CMake build for Fillscope itself, reusing the lockstep experiment's test setup
(GoogleTest, the Dear ImGui Test Engine, the sanitizer build and the mutation check; see
[its README](../experiments/lockstep/README.md)), plus CI on Linux running the tests on every push.

## 2. The frame channel and viewer shell
The experiment's engine-to-viewer path turned into a reusable library with tests, using stand-in
frames until the message interfaces exist.

## 3. Publishing the debugger's state
A VS Code extension, like HRW's, that writes the stack and locals to a file on every stop, so the
assistant can explain what is being stepped through. The one piece of HRW's debugger
integration Fillscope still needs (see the design note).
