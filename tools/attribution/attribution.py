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
# In a fork of someone else's repository (Fillscope's back end), a third kind of line: upstream's,
# any line last changed by a commit in upstream's history. The fork keeps a copy of this script:
#
#   python3 -I tools/attribution/attribution.py --report ATTRIBUTION.md --upstream REV
#   python3 -I tools/attribution/attribution.py --report ATTRIBUTION.md --check
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
UPSTREAM_IN_REPORT = re.compile(r"^\*\*Upstream:\*\* commit `([0-9a-f]{40})`", re.MULTILINE)
# Where a fork's report points for the rules (it has no AI-USE.md of its own).
FILLSCOPE_AI_USE = "https://github.com/dougdew64/fillscope/blob/main/docs/AI-USE.md"


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
    return sorted(text_file_lines(root, commit))


def text_file_lines(root, commit):
    """text_files(), each with its number of lines."""
    empty_tree = subprocess.run(["git", "-C", str(root), "hash-object", "-t", "tree", "--stdin"],
                                input=b"", capture_output=True, check=True).stdout.decode().strip()
    out = git(root, "diff", "--numstat", "-z", "--no-renames", empty_tree, commit)
    regular = set()
    for entry in git(root, "ls-tree", "-r", "-z", commit).split("\0"):
        if entry:
            mode, _, rest = entry.partition(" ")
            if mode in ("100644", "100755"):
                regular.add(rest.split("\t", 1)[1])
    files = {}
    for entry in out.split("\0"):
        if not entry:
            continue
        added, _, path = entry.split("\t", 2)
        if added != "-" and path in regular:
            files[path] = int(added)
    return files


def blamed_commits(root, commit, path):
    """The commit each line of `path` at `commit` is credited to, line by line. Whitespace-only
    changes (-w) don't take a line from its author, nor does moving or copying it from another file
    (-M -C)."""
    for line in git(root, "blame", "--porcelain", "-w", "-M", "-C", commit, "--", path).splitlines():
        match = BLAME_LINE.match(line)
        if match:
            yield match.group(1)


def blame_counts(root, commit, path, coauthored):
    """(owner's lines, co-authored lines) in `path` at `commit`."""
    owner = coauthor = 0
    for sha in blamed_commits(root, commit, path):
        if sha in coauthored:
            coauthor += 1
        else:
            owner += 1
    return owner, coauthor


def blame_counts_with_upstream(root, commit, path, coauthored, upstream):
    """(upstream lines, owner's lines, co-authored lines) in `path` at `commit`; a line whose commit
    is in `upstream` is upstream's."""
    counts = [0, 0, 0]
    for sha in blamed_commits(root, commit, path):
        counts[0 if sha in upstream else 2 if sha in coauthored else 1] += 1
    return tuple(counts)


def counted_files(root, commit, report_path):
    """(the files whose lines are counted, each with its number of lines; the files nobody wrote):
    every text file at `commit` but the report itself and those in NOT_WRITTEN."""
    counted, not_written = {}, []
    for path, lines in sorted(text_file_lines(root, commit).items()):
        if path == report_path:
            continue
        if path in NOT_WRITTEN:
            not_written.append(path)
            continue
        counted[path] = lines
    return counted, not_written


def share(part, whole):
    return f"{100 * part / whole:.1f}%" if whole else "-"


def report(root, rev, report_path=REPORT, upstream=None):
    """The report for `rev`, as Markdown; with `upstream`, a fork's (upstream_report)."""
    if upstream is not None:
        return upstream_report(root, rev, report_path, upstream)
    commit = git(root, "rev-parse", "--verify", f"{rev}^{{commit}}").strip()
    date, subject = git(root, "show", "-s", "--format=%cs%x00%s", commit).rstrip("\n").split("\0", 1)
    coauthored = coauthored_commits(root, commit)
    rows = []
    totals = {"code": [0, 0], "documents": [0, 0]}
    counted, not_written = counted_files(root, commit, report_path)
    for path in counted:
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


def upstream_report(root, rev, report_path, upstream_rev):
    """A fork's report: each line upstream's, the owner's or co-authored. Files unchanged since
    upstream are upstream's throughout, and are counted without blaming them."""
    commit = git(root, "rev-parse", "--verify", f"{rev}^{{commit}}").strip()
    upstream = git(root, "rev-parse", "--verify", f"{upstream_rev}^{{commit}}").strip()
    date, subject = git(root, "show", "-s", "--format=%cs%x00%s", commit).rstrip("\n").split("\0", 1)
    coauthored = coauthored_commits(root, commit)
    upstream_commits = set(git(root, "rev-list", upstream).split())
    changed = set(git(root, "diff", "--name-only", "-z", "--no-renames", upstream, commit).split("\0"))
    totals = {"code": [0, 0, 0], "documents": [0, 0, 0]}
    rows = []
    upstream_only_files = upstream_only_lines = 0
    counted, not_written = counted_files(root, commit, report_path)
    for path, lines in counted.items():
        if path in changed:
            counts = blame_counts_with_upstream(root, commit, path, coauthored, upstream_commits)
        else:
            counts = (lines, 0, 0)
        kind = "documents" if path.endswith(".md") else "code"
        for i in range(3):
            totals[kind][i] += counts[i]
        if counts[1] or counts[2]:
            rows.append((path, *counts))
        else:
            upstream_only_files += 1
            upstream_only_lines += counts[0]

    out = [
        "# Attribution",
        "",
        "**Purpose:** who wrote each file's lines: upstream (the repository this was forked from),",
        "Fillscope's owner, by hand, or Claude, co-authoring",
        f"([Fillscope's AI-USE.md]({FILLSCOPE_AI_USE})).",
        f"**Status:** record, generated by `{SCRIPT}`; not edited by hand. **Read when:** checking",
        "what this fork changed, and who wrote the changes.",
        "",
        f"As of commit `{commit}` ({date}, \"{subject}\").",
        "A report can't describe the commit that contains it, since only committed lines can be",
        "credited, so it names the commit it describes. To check it, run",
        f"`python3 -I {SCRIPT} --report {report_path} --check`.",
        "",
        f"**Upstream:** commit `{upstream}` and its ancestors, whose lines are",
        "upstream's.",
        "",
        "**How lines are counted:** `git blame -w -M -C` credits each line to the commit that last",
        "changed it, ignoring whitespace-only changes and following lines moved or copied from other",
        "files. A commit in upstream's history is upstream's; of the rest, one with a",
        "`Co-Authored-By: Claude` trailer is co-authored, and any other is the owner's. No commit is",
        "both the owner's and co-authored.",
        "",
        "| | Upstream lines | Owner's lines | Co-authored lines |",
        "|---|---:|---:|---:|",
    ]
    for kind, label in (("code", "Code, tests and configuration"), ("documents", "Documents (`.md`)")):
        up, owner, coauthor = totals[kind]
        out.append(f"| {label} | {up} | {owner} | {coauthor} |")
    out += ["", "## Files with lines not from upstream", "",
            "| File | Upstream lines | Owner's lines | Co-authored lines |", "|---|---:|---:|---:|"]
    out += [f"| `{path}` | {up} | {owner} | {coauthor} |" for path, up, owner, coauthor in rows]
    out += ["", f"The other {upstream_only_files} files are upstream's throughout: {upstream_only_lines} lines."]
    if not_written:
        out += ["", "## Not counted", "", "| File | Why |", "|---|---|"]
        out += [f"| `{path}` | {NOT_WRITTEN[path]} |" for path in not_written]
    return "\n".join(out) + "\n"


def check(root, report_path=REPORT):
    """Why the committed report isn't exactly what its named commit gives, or None if it is. A
    fork's report names its upstream too, and is regenerated with it."""
    path = root / report_path
    if not path.exists():
        return f"{report_path} doesn't exist"
    text = path.read_text()
    named = COMMIT_IN_REPORT.search(text)
    if named is None:
        return f"{report_path} names no commit"
    commit = named.group(1)
    ancestor = subprocess.run(["git", "-C", str(root), "merge-base", "--is-ancestor", commit, "HEAD"],
                              capture_output=True, check=False)
    if ancestor.returncode != 0:
        return f"{report_path} names commit {commit}, which isn't in HEAD's history"
    upstream = UPSTREAM_IN_REPORT.search(text)
    upstream = upstream.group(1) if upstream else None
    if report(root, commit, report_path, upstream) != text:
        how = f"--report {report_path} --at {commit}" + (f" --upstream {upstream}" if upstream else "")
        if report_path == REPORT and upstream is None:
            how = f"--at {commit}"
        return f"{report_path} isn't what commit {commit} gives: regenerate it with `python3 -I {SCRIPT} {how}`"
    return None


def main(argv):
    parser = argparse.ArgumentParser(description="Per file, the owner's lines and the lines co-authored with Claude.")
    parser.add_argument("--at", default="HEAD", metavar="REV", help="the commit to describe (default HEAD)")
    parser.add_argument("--check", action="store_true", help="check the report against the commit it names")
    parser.add_argument("--report", default=REPORT, help=f"the report's path in the repository (default {REPORT})")
    parser.add_argument("--upstream", metavar="REV",
                        help="in a fork: the commit forked from; its history's lines are upstream's")
    parser.add_argument("--root", default=str(ROOT), help=argparse.SUPPRESS)  # for the tests
    args = parser.parse_args(argv)
    root = pathlib.Path(args.root)
    try:
        if args.check:
            problem = check(root, args.report)
            if problem:
                print(f"attribution: {problem}", file=sys.stderr)
                return 1
            print(f"attribution: {args.report} matches its commit")
            return 0
        text = report(root, args.at, args.report, args.upstream)
        (root / args.report).parent.mkdir(parents=True, exist_ok=True)
        (root / args.report).write_text(text)
        print(f"attribution: wrote {args.report}")
        return 0
    except Error as error:
        print(f"attribution: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
