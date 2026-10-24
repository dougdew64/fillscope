# The code map (docs/CODE-MAP.md) names every part of the code: the files that make it up, where the
# architecture documents explain it, and when the owner asked about it. This checks that it names
# every file: each code file in the repository belongs to exactly one part, and each file pattern a
# part lists matches at least one file. Without it, a new file would be missing from the map
# without anyone noticing, and the map would claim more of the code is accounted for than is.
#
#   python3 -I tools/code_map/code_map.py --check [ROOT]
#
# The map's tables: each section heading (##) may name a directory in backticks, which its rows'
# patterns are relative to; each row's second cell lists its file patterns, each in backticks. A
# pattern ending in / is a directory and everything under it; any other is a file name, where * and
# ? match as in a shell (fnmatch).
import argparse
import fnmatch
import pathlib
import re
import subprocess
import sys

MAP = pathlib.Path("docs") / "CODE-MAP.md"
# The back end is the fork of the whole book; only its last chapter is built and run.
BACKEND = "backend/"
BACKEND_IN_USE = "backend/Chapter12/"
# Not code: the documents (which explain the code rather than being it) and the license.
NOT_CODE = ("*.md", "LICENSE")


def repository_files(root):
    """Every file of the working tree: tracked, or untracked and not ignored, and present now,
    including a submodule's (backend/), as paths from `root`."""
    def listed(directory):
        output = subprocess.run(["git", "-C", str(directory), "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
                                stdout=subprocess.PIPE, check=True).stdout.decode()
        return [name for name in output.split("\0") if name]

    files = []
    for name in listed(root):
        path = root / name
        if path.is_dir() and not path.is_symlink():
            files.extend(f"{name}/{inner}" for inner in listed(path) if (path / inner).is_file())
        elif path.is_file():
            files.append(name)
    return sorted(files)


def is_code(path):
    """Whether `path` is one the map must account for."""
    if any(fnmatch.fnmatchcase(pathlib.PurePosixPath(path).name, pattern) for pattern in NOT_CODE):
        return False
    return not path.startswith(BACKEND) or path.startswith(BACKEND_IN_USE)


def parse_map(text):
    """The map's parts, as (part, [pattern, ...]), each pattern a path from the repository's root."""
    parts = []
    base = ""
    for line in text.splitlines():
        if line.startswith("## "):
            named = re.search(r"`([^`]+/)`", line)
            base = named.group(1) if named else ""
            continue
        if not line.startswith("|"):
            continue
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        if len(cells) < 2 or cells[0] in ("Part", "") or set(cells[0]) <= set("-: "):
            continue
        patterns = [base + pattern for pattern in re.findall(r"`([^`]+)`", cells[1])]
        parts.append((cells[0], patterns))
    return parts


def matches(pattern, path):
    if pattern.endswith("/"):
        return path.startswith(pattern)
    return fnmatch.fnmatchcase(path, pattern)


def problems(parts, files):
    """What is wrong with the map, given the repository's files: code files no part names, or more
    than one does, and patterns that name no file."""
    found = []
    code = [path for path in files if is_code(path)]
    for part, patterns in parts:
        if not patterns:
            found.append(f"{part}: lists no files")
        for pattern in patterns:
            if not any(matches(pattern, path) for path in code):
                found.append(f"{part}: `{pattern}` names no code file")
    for path in code:
        owners = [part for part, patterns in parts if any(matches(pattern, path) for pattern in patterns)]
        if not owners:
            found.append(f"{path}: in no part")
        elif len(owners) > 1:
            found.append(f"{path}: in more than one part ({', '.join(owners)})")
    return found


def main(argv):
    parser = argparse.ArgumentParser(description="Check that the code map names every code file exactly once.")
    parser.add_argument("--check", action="store_true", required=True)
    parser.add_argument("root", nargs="?", type=pathlib.Path, default=pathlib.Path(__file__).resolve().parents[2])
    args = parser.parse_args(argv)
    parts = parse_map((args.root / MAP).read_text())
    found = problems(parts, repository_files(args.root))
    for problem in found:
        print(problem, file=sys.stderr)
    if found:
        print(f"{MAP}: {len(found)} problem{'s' if len(found) != 1 else ''}", file=sys.stderr)
        return 1
    print(f"{MAP}: every code file is in exactly one part")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
