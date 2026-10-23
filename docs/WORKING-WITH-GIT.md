# Working with git

**Purpose:** the git and GitHub steps for working on Fillscope and its back end, explained for
someone new to git. **Status:** procedure. **Read when:** changing the back end, committing,
moving between machines, or when git shows something unexpected.

## The ideas
- **A repository** is a project folder with its whole history. There are two:
  - **Fillscope:** `~/dev/fillscope`, on GitHub as `dougdew64/fillscope`.
  - **The fork** of the trading system in Ghosh's book: `~/dev/fillscope/backend`, on GitHub as
    `dougdew64/Building-Low-Latency-Applications-with-CPP`.
- **A commit** is a saved snapshot of changes, with a message saying what changed and why. It
  exists only on your machine until it is pushed.
- **A branch** is a named line of commits. In the fork, `main` mirrors the book's original code,
  and every change goes on the `fillscope` branch.
- **Push** sends your commits to GitHub; **pull** fetches commits from GitHub, such as ones made on
  your other machine.
- **The submodule:** Fillscope doesn't contain the back end's code. It holds a pointer to one exact
  commit of the fork (the "pin"). So a change to the back end takes two commits: one in the fork,
  for the change itself, then one in Fillscope, to move the pin to it.

## Starting a session, or moving to another machine
```sh
cd ~/dev/fillscope
git pull --recurse-submodules    # Fillscope's new commits, and backend/ moved to its pin
```
The first time on a machine, after cloning Fillscope, fetch the back end once:
`git submodule update --init`.

## Changing the back end
1. **Get onto the branch.** After a pull, `backend/` sits on the pinned commit with no branch
   checked out ("detached HEAD"). Commits made there belong to no branch and are easily lost, so
   always start with:
   ```sh
   cd ~/dev/fillscope/backend
   git switch fillscope    # onto the branch
   git pull                # its latest commits
   ```
2. **Edit, build and test** as usual ([RUNNING.md](RUNNING.md)). Fillscope's build compiles
   whatever is in `backend/` at that moment.
3. **Commit and push in the fork:**
   ```sh
   git status                    # what changed
   git diff                      # the changes themselves
   git add Chapter12/exchange/matcher/matching_engine.cpp   # each file to include
   git commit -m "Say what changed and why"
   git push
   ```
4. **Move Fillscope's pin to the new commit:**
   ```sh
   cd ~/dev/fillscope
   git status                    # shows: modified: backend (new commits)
   git add backend
   git commit -m "Pin the back end at the commit that ..."
   git push
   ```
   Until this commit is pushed, CI and your other machine still build the old pin.

**Your commits and attribution:** a commit with no `Co-Authored-By: Claude` line is counted as
yours in [ATTRIBUTION.md](ATTRIBUTION.md), and `git commit` as written adds none. Keep your
changes and the assistant's in separate commits ([AI-USE.md](AI-USE.md)).

## Seeing where you are
These only look; they are always safe to run.
- `git status`: the branch you're on, what has changed, and what is staged for the next commit.
- `git log --oneline -5`: the last five commits.
- `git diff`: the changes not yet staged; `git diff --staged`: the ones that are.

## In VS Code
The **Source Control** panel (the branch icon on the left) does the same with buttons, and lists
Fillscope and `backend/` as two repositories. The branch is shown at the bottom left. If it shows
a commit id instead of `fillscope` while you're in `backend/`, do step 1 above first.

## When something looks wrong
Stop, and look before fixing: run `git status` and read what it says, or ask the assistant with
its output. Nearly everything in git can be undone, but a few commands throw work away, among them
`git reset --hard`, `git checkout -- <file>`, `git restore <file>`, `git clean` and
`git push --force`. Don't run those until you know exactly what they discard.
