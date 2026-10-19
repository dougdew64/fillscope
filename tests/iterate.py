# Between edits: builds the debug build, then runs only the fast tests whose programs are affected
# by what changed since their last passing run. Before a commit, run tests/run-tests.sh instead:
# it runs everything, in both builds.
#
#   python3 -I tests/iterate.py              the affected fast test programs
#   python3 -I tests/iterate.py --all        every fast test program
#   python3 -I tests/iterate.py -R REGEX     fast tests whose names match REGEX (CTest -R)
#   python3 -I tests/iterate.py --list       which programs are affected, without running them
#   python3 -I tests/iterate.py --record-all mark every program as passed (run-tests.sh does this)
#
# "Affected" comes from the build, not from a hand-kept map: each test program, and each file it
# uses at run time, is listed in build/debug/test-manifest.txt (written by CMakeLists.txt). A
# program is affected when its binary, one of those files, or CMakeLists.txt differs from its last
# passing run. A change the build doesn't propagate into a program can't change its result.
import hashlib
import json
import os
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
BUILD = ROOT / "build" / "debug"
MANIFEST = BUILD / "test-manifest.txt"
# tests/selection_check.py points this elsewhere, so checking the selection never touches the real
# record of what has passed.
STATE = pathlib.Path(os.environ.get("FILLSCOPE_ITERATE_STATE", BUILD / "last-pass.json"))
FAST_TIERS = {"unit", "ui"}


def build():
    subprocess.run(["cmake", "-S", str(ROOT), "-B", str(BUILD), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Debug"],
                   check=True, stdout=subprocess.DEVNULL)
    subprocess.run(["cmake", "--build", str(BUILD)], check=True)


def programs():
    """[(name, tier, [files])] from the manifest."""
    result = []
    for line in MANIFEST.read_text().splitlines():
        if line.strip():
            name, tier, *files = line.split()
            result.append((name, tier, files))
    return result


def fingerprint(files):
    digest = hashlib.sha256()
    for path in [*files, str(ROOT / "CMakeLists.txt")]:
        digest.update(path.encode())
        digest.update(hashlib.sha256(pathlib.Path(path).read_bytes()).digest())
    return digest.hexdigest()


def load_state():
    return json.loads(STATE.read_text()) if STATE.exists() else {}


def record(names, everything):
    state = load_state()
    for name, _, files in everything:
        if name in names:
            state[name] = fingerprint(files)
    STATE.write_text(json.dumps(state, indent=1, sort_keys=True) + "\n")


def affected(everything):
    state = load_state()
    return {name for name, _, files in everything if state.get(name) != fingerprint(files)}


def ctest(args):
    """Runs CTest in parallel, prints its output and the five slowest tests; returns success."""
    command = ["ctest", "--test-dir", str(BUILD), "-j", str(os.cpu_count() or 1), "--no-tests=error",
               "--output-on-failure", *args]
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    print(result.stdout, end="")
    times = re.findall(r"Test +#\d+: (\S+) \.+ +\S+ +([\d.]+) sec", result.stdout)
    slowest = sorted(times, key=lambda t: float(t[1]), reverse=True)[:5]
    if slowest:
        print("slowest: " + ", ".join(f"{name} {seconds}s" for name, seconds in slowest))
    return result.returncode == 0


def exe_labels(names):
    return ["-L", "^exe:(" + "|".join(sorted(names)) + ")$"]


def main(args):
    if args == ["--record-all"]:
        everything = programs()
        record({name for name, _, _ in everything}, everything)
        return 0

    build()
    everything = programs()
    fast = {name for name, tier, _ in everything if tier in FAST_TIERS}
    slow = {name for name, tier, _ in everything if tier not in FAST_TIERS}

    if args == ["--list"]:
        changed = affected(everything)
        for name, tier, _ in everything:
            print(f"{'affected ' if name in changed else 'unchanged'} {tier:9} {name}")
        return 0

    if args and args != ["--all"]:  # a filter: run what it names, record nothing
        return 0 if ctest([*args, *exe_labels(fast)]) else 1

    changed = affected(everything)
    selected = fast if args == ["--all"] else fast & changed
    if changed & slow:
        print("affected, slow tier (run tests/run-tests.sh): " + ", ".join(sorted(changed & slow)))
    if not selected:
        print("no fast test program affected since its last passing run")
        return 0
    print("running: " + ", ".join(sorted(selected)))
    if not ctest(exe_labels(selected)):
        return 1
    record(selected, everything)
    return 0


sys.exit(main(sys.argv[1:]))
