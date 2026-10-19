# Plan

**Purpose:** the next steps, in order, and what "done" means for each. **Status:** live plan; a
step is removed when it lands (git keeps it), and the file is deleted when it's empty.
**Read when:** choosing what to work on next.

Decisions behind the steps are in the [charter](CHARTER.md); the lockstep design and its
experiment results are in [DEBUGGER-LOCKSTEP.md](DEBUGGER-LOCKSTEP.md). In parallel, the owner
reads the trading domain; the stage-by-stage split of who writes what (charter §4) waits on that.

## 1. CI
The top-level build and the test setup exist ([RUNNING.md](RUNNING.md)). What's left of the
skeleton: CI on Linux running `tests/run-tests.sh` on every push, and the checks of the tests
themselves (`mutants.py`, `selection_check.py`). The debugger tests need GDB and LLDB on the CI
machine, or they are skipped there and the run says so.

## 2. The viewer shell
The viewer grown from the experiment's one window into the shell later views live in, still on
stand-in frames until the message interfaces exist. The channel (`src/channel/`) is already a
library with tests.

## 3. Publishing the debugger's state
A VS Code extension that writes the stack and locals to a file on every stop, so the assistant can
explain what is being stepped through. Its requirements are in
[DEBUGGER-LOCKSTEP.md](DEBUGGER-LOCKSTEP.md).
