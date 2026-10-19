# Plan

**Purpose:** the next steps, in order, and what "done" means for each. **Status:** live plan; a
step is removed when it lands (git keeps it), and the file is deleted when it's empty.
**Read when:** choosing what to work on next.

Decisions behind the steps are in the [charter](CHARTER.md); the lockstep design and its
experiment results are in [DEBUGGER-LOCKSTEP.md](DEBUGGER-LOCKSTEP.md). In parallel, the owner
reads the trading domain; the stage-by-stage split of who writes what (charter §4) waits on that.

## 1. Publishing the debugger's state
A VS Code extension that writes the stack and locals to a file on every stop, so the assistant can
explain what is being stepped through. Its requirements are in
[DEBUGGER-LOCKSTEP.md](DEBUGGER-LOCKSTEP.md).

## 2. The viewer shell
The viewer grown from its one window into the shell later views live in. **Generic parts first,
after a design discussion:** the context bar and the plumbing that captures what is pointed at for
the assistant. The views of the domain (the book, an order's history) wait on the message
interfaces, which wait on the owner's reading.
