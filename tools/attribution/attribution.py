# Attribution: for each file, how many of its lines come from the owner's commits and how many from
# commits co-authored with Claude (docs/AI-USE.md). A commit is co-authored when it carries a
# `Co-Authored-By: Claude ...` trailer, and the owner's otherwise; the rule that no commit mixes the
# two is what makes this count mean something.
#
#   python3 -I tools/attribution/attribution.py              write docs/ATTRIBUTION.md for HEAD
#   python3 -I tools/attribution/attribution.py --at REV     write it for another commit
#   python3 -I tools/attribution/attribution.py --check      check docs/ATTRIBUTION.md is exactly
#                                                            what its named commit gives
#
# The report can never describe the commit that contains it (git blame credits only committed
# lines), so it names the commit it describes, and anyone can regenerate it there.
import argparse
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
REPORT = "docs/ATTRIBUTION.md"
SCRIPT = "tools/attribution/attribution.py"

# Files neither the owner nor Claude wrote, listed apart and not counted.
NOT_WRITTEN = {
    "LICENSE": "the standard text of the MIT license",
    "tools/vscode-debug-state/package-lock.json": "written by npm",
}

# The line before each blamed line in `git blame --porcelain`: the commit, the line's number in
# that commit's file and in this one, and (on a group's first line) the group's size. Content
# lines start with a tab, and no other header line starts with 40 hexadecimal digits.
BLAME_LINE = re.compile(r"^([0-9a-f]{40}) \d+ \d+(?: \d+)?$")
COMMIT_IN_REPORT = re.compile(r"^As of commit `([0-9a-f]{40})`", re.MULTILINE)


class Error(Exception):
    pass


def git(root, *args, binary=False):
    result = subprocess.run(["git", "-C", str(root), *args], capture_output=True, check=False)
    if result.returncode != 0:
        raise Error(f"git {' '.join(args)}: {result.stderr.decode(errors='replace').strip()}")
    return result.stdout if binary else result.stdout.decode()


def is_coauthor_trailer(value):
    """A Co-Authored-By trailer's value names Claude: `Claude <address>` or `Claude Model <address>`."""
    return value == "Claude" or value.startswith("Claude ")


def coauthored_commits(root, commit):
    """The full hashes of the co-authored commits among `commit` and its ancestors. Git matches the
    trailer's key without regard to case, as trailers are defined."""
    out = git(root, "log", "--format=%H%x00%(trailers:key=Co-Authored-By,valueonly,unfold,separator=%x01)%x00",
              commit)
    result = set()
    fields = out.split("\0")
    for sha, trailers in zip(fields[0::2], fields[1::2]):
        values = [value.strip() for value in trailers.split("\x01")]
        if any(is_coauthor_trailer(value) for value in values):
            result.add(sha.strip())
    return result


def text_files(root, commit):
    """The regular files at `commit` whose contents are text, by path. Git's own test decides what
    is binary (a diff against the empty tree reports a binary file's line counts as '-')."""
    empty_tree = subprocess.run(["git", "-C", str(root), "hash-object", "-t", "tree", "--stdin"],
                                input=b"", capture_output=True, check=True).stdout.decode().strip()
    out = git(root, "diff", "--numstat", "-z", "--no-renames", empty_tree, commit)
    regular = set()
    for entry in git(root, "ls-tree", "-r", "-z", commit).split("\0"):
        if entry:
            mode, _, rest = entry.partition(" ")
            if mode in ("100644", "100755"):
                regular.add(rest.split("\t", 1)[1])
    files = []
    for entry in out.split("\0"):
        if not entry:
            continue
        added, _, path = entry.split("\t", 2)
        if added != "-" and path in regular:
            files.append(path)
    return sorted(files)


def blame_counts(root, commit, path, coauthored):
    """(owner's lines, co-authored lines) in `path` at `commit`. Whitespace-only changes (-w) don't
    take a line from its author, nor does moving or copying it from another file (-M -C)."""
    owner = coauthor = 0
    for line in git(root, "blame", "--porcelain", "-w", "-M", "-C", commit, "--", path).splitlines():
        match = BLAME_LINE.match(line)
        if match:
            if match.group(1) in coauthored:
                coauthor += 1
            else:
                owner += 1
    return owner, coauthor


def share(part, whole):
    return f"{100 * part / whole:.1f}%" if whole else "-"


def report(root, rev):
    """The report for `rev`, as Markdown."""
    commit = git(root, "rev-parse", "--verify", f"{rev}^{{commit}}").strip()
    date, subject = git(root, "show", "-s", "--format=%cs%x00%s", commit).rstrip("\n").split("\0", 1)
    coauthored = coauthored_commits(root, commit)
    rows = []
    totals = {"code": [0, 0], "documents": [0, 0]}
    not_written = []
    for path in text_files(root, commit):
        if path == REPORT:
            continue
        if path in NOT_WRITTEN:
            not_written.append(path)
            continue
        owner, coauthor = blame_counts(root, commit, path, coauthored)
        kind = "documents" if path.endswith(".md") else "code"
        totals[kind][0] += owner
        totals[kind][1] += coauthor
        rows.append((path, owner, coauthor))

    out = [
        "# Attribution",
        "",
        "**Purpose:** who wrote each file's lines: the owner, by hand, or Claude, co-authoring",
        f"([AI-USE.md](AI-USE.md)). **Status:** record, generated by `{SCRIPT}`; not edited by hand.",
        "**Read when:** checking how much of Fillscope is hand-written, and where.",
        "",
        f"As of commit `{commit}` ({date}, \"{subject}\"). A report can't describe the commit that",
        "contains it, since only committed lines can be credited, so it names the commit it describes. To",
        f"check it, run `python3 -I {SCRIPT} --check`, or regenerate it at that commit with `--at`.",
        "",
        "**How lines are counted:** `git blame -w -M -C` credits each line to the commit that last",
        "changed it, ignoring whitespace-only changes and following lines moved or copied from other",
        "files. A commit with a `Co-Authored-By: Claude` trailer is co-authored; any other is the",
        "owner's. (A commit's author field doesn't decide it: it names the owner's git identity either",
        "way.) No commit is both: a change Claude suggests to the owner's code is typed by the owner",
        "or committed on its own, co-authored.",
        "",
        "| | Owner's lines | Co-authored lines | Owner's share |",
        "|---|---:|---:|---:|",
    ]
    for kind, label in (("code", "Code, tests and configuration"), ("documents", "Documents (`.md`)")):
        owner, coauthor = totals[kind]
        out.append(f"| {label} | {owner} | {coauthor} | {share(owner, owner + coauthor)} |")
    out += ["", "## By file", "", "| File | Owner's lines | Co-authored lines |", "|---|---:|---:|"]
    out += [f"| `{path}` | {owner} | {coauthor} |" for path, owner, coauthor in rows]
    if not_written:
        out += ["", "## Not counted", "", "| File | Why |", "|---|---|"]
        out += [f"| `{path}` | {NOT_WRITTEN[path]} |" for path in not_written]
    return "\n".join(out) + "\n"


def check(root):
    """Why the committed report isn't exactly what its named commit gives, or None if it is."""
    path = root / REPORT
    if not path.exists():
        return f"{REPORT} doesn't exist"
    text = path.read_text()
    named = COMMIT_IN_REPORT.search(text)
    if named is None:
        return f"{REPORT} names no commit"
    commit = named.group(1)
    ancestor = subprocess.run(["git", "-C", str(root), "merge-base", "--is-ancestor", commit, "HEAD"],
                              capture_output=True, check=False)
    if ancestor.returncode != 0:
        return f"{REPORT} names commit {commit}, which isn't in HEAD's history"
    if report(root, commit) != text:
        return f"{REPORT} isn't what commit {commit} gives: regenerate it with `python3 -I {SCRIPT} --at {commit}`"
    return None


def main(argv):
    parser = argparse.ArgumentParser(description="Per file, the owner's lines and the lines co-authored with Claude.")
    parser.add_argument("--at", default="HEAD", metavar="REV", help="the commit to describe (default HEAD)")
    parser.add_argument("--check", action="store_true", help=f"check {REPORT} against the commit it names")
    parser.add_argument("--root", default=str(ROOT), help=argparse.SUPPRESS)  # for the tests
    args = parser.parse_args(argv)
    root = pathlib.Path(args.root)
    try:
        if args.check:
            problem = check(root)
            if problem:
                print(f"attribution: {problem}", file=sys.stderr)
                return 1
            print(f"attribution: {REPORT} matches its commit")
            return 0
        text = report(root, args.at)
        (root / REPORT).parent.mkdir(parents=True, exist_ok=True)
        (root / REPORT).write_text(text)
        print(f"attribution: wrote {REPORT}")
        return 0
    except Error as error:
        print(f"attribution: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
