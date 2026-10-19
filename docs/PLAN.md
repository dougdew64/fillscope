# Plan

**Purpose:** the next steps, in order, and what "done" means for each. **Status:** live plan; a
step is removed when it lands (git keeps it), and the file is deleted when it's empty.
**Read when:** choosing what to work on next.

Decisions behind the steps are in the [charter](CHARTER.md); the lockstep design and its
experiment results are in [DEBUGGER-LOCKSTEP.md](DEBUGGER-LOCKSTEP.md). In parallel, the owner
reads the trading domain; the stage-by-stage split of who writes what (charter §4) waits on that.

## 1. The viewer shell
The viewer grown from its one window into the shell later views live in. **Generic parts first:**
the context bar and the plumbing that captures what is pointed at and followed for the assistant,
on stand-in frames with a stand-in `id` to follow (rulings in [DECISIONS.md](DECISIONS.md),
2026-10-19). **Built and tested ([RUNNING.md](RUNNING.md), "Pointing and following"); left: the
owner's test,** pointing and following in the viewer and asking the assistant about it. The views
of the domain (the book, an order's history) wait on the message interfaces, which wait on the
owner's reading.

## 2. The tech debt backlog, and exemplary code throughout
Work through [TECH-DEBT.md](TECH-DEBT.md), from the first sweep, and bring all the code to the
standard of the context capture: exemplary contemporary C++, highly commented, since it is code to
learn from (charter §1). Its section F lists the code-quality items found so far.
