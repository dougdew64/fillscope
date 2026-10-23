# CLAUDE.md

**Purpose:** how Claude works in this repository. **Status:** procedure, read at the start of every
session. **Read when:** always, first.

The owner's decisions are in [docs/CHARTER.md](docs/CHARTER.md); the record is
[docs/DECISIONS.md](docs/DECISIONS.md).

## This repository is public
Everything committed is published, history included. So:
- Write every document and commit message for a public reader.
- Nothing personal beyond what a professional profile would show; no companies' names unless they
  are public sources being cited.
- No secrets, ever; `.gitignore` covers them. No market data files.
- Nothing here links to material that isn't public.

## Who writes what
The owner **hand-writes the code that demonstrates modern C++ design**; Claude reviews it, catches
defects and suggests, but doesn't write it. Claude writes everything else, and **tests all the
code aggressively, the owner's included**. Which code is the owner's is decided stage by stage
(charter §4); until a stage's split is decided, Claude asks before writing any of that stage's code.

**Attribution is mechanical** ([docs/AI-USE.md](docs/AI-USE.md)): a commit is the owner's (no
trailer) or co-authored (`Co-Authored-By: Claude`), never both. Every commit Claude makes carries
the trailer, and Claude never folds a change into the owner's commit: a suggested change to the
owner's code is typed by the owner, or committed on its own, co-authored. In every commit it makes, Claude first
regenerates [docs/ATTRIBUTION.md](docs/ATTRIBUTION.md) (`python3 -I tools/attribution/attribution.py`),
so the report describes the commit before.

## Every session
1. `git pull`; commit and push every commit at once, without asking.
2. Use `date` for today's date.

## Documents
Every document opens with three lines: Purpose, Status, Read when. Status is one of **authority**,
**procedure**, **reference**, **live plan** (deleted when its work lands), **record** or
**historical**. One owner per fact; others link to it. A conclusion from conversation is a ruling
for the record, not a new rule; when uncertain, ask the owner.

When the owner asks how or why the code does something, answer, then write the answer into
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) in the same session, rewriting what is there rather
than appending; move the fact there if another document owned it.

## Craft
- **Verify, never recite:** build and run anything claimed to work; name the compiler version.
- **Accuracy over appearance:** a display or explanation that isn't exactly true is a defect
  (charter §2).
- **Edit with the Edit tool;** check the diff of any scripted multi-file edit before committing.
