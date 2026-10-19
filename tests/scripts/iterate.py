# Between edits: builds the debug build, then runs only the fast tests whose programs are affected
# by what changed since their last passing run. Before a commit, run tests/scripts/run-tests.sh
# instead: it runs everything, in both builds.
#
#   python3 -I tests/scripts/iterate.py              the affected fast test programs
#   python3 -I tests/scripts/iterate.py --all        every fast test program
#   python3 -I tests/scripts/iterate.py -R REGEX     fast tests whose names match REGEX (CTest -R)
#   python3 -I tests/scripts/iterate.py --list       which programs are affected, without running
#
# For run-tests.sh and the checks of the tests:
#   --snapshot FILE                     write every program's fingerprint, as the files are now
#   --record-snapshot FILE CTEST_LOG    record the snapshot's programs as passed, except any whose
#                                       tests were all skipped in CTEST_LOG (CTest's output)
#   --record-all                        record every program as passed, as the files are now
#   --check-registration                every CTest test belongs to a program in the manifest
#
# FILLSCOPE_ITERATE_BUILD and FILLSCOPE_ITERATE_STATE choose another build directory and pass
# record; the other FILLSCOPE_ variables configure the tests, so they count in the fingerprint.
#
# "Affected" comes from the build, not from a hand-kept map: each test program, and each file it
# uses at run time, is listed in build/debug/test-manifest.txt (written by tests/CMakeLists.txt). A
# program is affected when its binary, one of those files, a shared library it loads, the Node.js
# that runs it, a FILLSCOPE_ environment variable, or any CMakeLists.txt differs from its last
# passing run. A change the build doesn't propagate into a program can't change its result.
#
# A program marked "always" in the manifest runs on every iteration: its result depends on files
# it can't list (docs_tests follows links to any file in the repository), and it is fast.
#
# Each program's fingerprint is taken before its tests run, and recorded only if they pass: a file
# edited while the tests ran is then seen as changed next time, rather than recorded as tested.
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
# run-tests.sh points this at the sanitizer build to check its registration; everything else uses
# the debug build.
BUILD = pathlib.Path(os.environ.get("FILLSCOPE_ITERATE_BUILD", ROOT / "build" / "debug"))
MANIFEST = BUILD / "test-manifest.txt"
# selection_check.py points this elsewhere, so checking the selection never touches the real
# record of what has passed.
STATE = pathlib.Path(os.environ.get("FILLSCOPE_ITERATE_STATE", BUILD / "last-pass.json"))
# Run between edits when affected. The debugger tier waits for run-tests.sh.
FAST_TIERS = {"unit", "ui", "check"}

USAGE = """usage: iterate.py [--all | --list | -R REGEX]
       iterate.py --snapshot FILE | --record-snapshot FILE CTEST_LOG | --record-all | --check-registration"""


class Program:
    """One line of the manifest: name, tier, flags, the program ("-" for a command), run-time files."""

    def __init__(self, line):
        # Tab-separated, so a path may contain spaces.
        self.name, self.tier, flags, program, *runtime = line.split("\t")
        self.always = flags == "always"
        self.binary = None if program == "-" else program
        self.files = ([] if self.binary is None else [self.binary]) + [f for f in runtime if f]

    @property
    def fast(self):
        return self.tier in FAST_TIERS


def build():
    """Configures and builds the debug build; False if either fails (the compiler said why)."""
    configure = ["cmake", "-S", str(ROOT), "-B", str(BUILD), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Debug"]
    return (subprocess.run(configure, stdout=subprocess.DEVNULL).returncode == 0 and
            subprocess.run(["cmake", "--build", str(BUILD)]).returncode == 0)


def programs():
    return [Program(line) for line in MANIFEST.read_text().splitlines() if line.strip()]


def build_files():
    """Every CMakeLists.txt of ours, tracked or new: a change to any can change how a test is run.
    Asking git leaves out every build directory and installed package, whatever its name."""
    listed = subprocess.run(["git", "-C", str(ROOT), "ls-files", "--cached", "--others", "--exclude-standard",
                             "-z", "--", "CMakeLists.txt", "*/CMakeLists.txt"],
                            stdout=subprocess.PIPE, check=True).stdout.decode()
    return sorted(str(ROOT / name) for name in set(listed.split("\0")) if name and (ROOT / name).exists())


def environment():
    """What a program's results can depend on beyond its files: the variables that configure our
    tests, and the Node.js that runs the extension's (its path, size and modification time)."""
    parts = [f"{key}={value}" for key, value in sorted(os.environ.items())
             if key.startswith("FILLSCOPE_") and not key.startswith("FILLSCOPE_ITERATE_")]
    node = shutil.which("node")
    if node:
        parts.append(stat_of(node))
    return "\n".join(parts)


def stat_of(path):
    s = os.stat(path)
    return f"{path} {s.st_size} {s.st_mtime_ns}"


def shared_libraries(binary):
    """The shared libraries `binary` loads, by path, size and modification time: an updated system
    library changes the fingerprint without being hashed whole."""
    output = subprocess.run(["ldd", binary], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True).stdout
    paths = sorted(set(re.findall(r"=> (/\S+)", output)))
    return [stat_of(p) for p in paths if os.path.exists(p)]


def fingerprint(program, shared):
    """`shared` is what every program depends on: the build files and the environment."""
    digest = hashlib.sha256(shared.encode())
    for path in program.files:
        digest.update(path.encode())
        digest.update(hashlib.sha256(pathlib.Path(path).read_bytes()).digest())
    if program.binary is not None:
        for line in shared_libraries(program.binary):
            digest.update(line.encode())
    return digest.hexdigest()


def fingerprints(selected):
    """{name: fingerprint} for each program in `selected`, as the files are now."""
    shared = hashlib.sha256()
    for path in build_files():
        shared.update(path.encode())
        shared.update(hashlib.sha256(pathlib.Path(path).read_bytes()).digest())
    shared = shared.hexdigest() + environment()
    return {p.name: fingerprint(p, shared) for p in selected}


def load_state():
    """The record of passing runs; empty, with a warning, if it is unreadable."""
    try:
        return json.loads(STATE.read_text()) if STATE.exists() else {}
    except (OSError, ValueError) as error:
        print(f"warning: ignoring the unreadable pass record {STATE}: {error}")
        return {}


def write_atomically(path, text):
    """Replaces `path` whole: written to a temporary file in the same directory, then renamed."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile("w", dir=path.parent, prefix=path.name, suffix=".tmp", delete=False) as out:
        out.write(text)
    os.replace(out.name, path)


def record(passed):
    """Records `passed` ({name: fingerprint}) as the latest passing runs."""
    state = load_state()
    state.update(passed)
    write_atomically(STATE, json.dumps(state, indent=1, sort_keys=True) + "\n")


def tests_by_program():
    """{program name: [CTest test names]}, from CTest's own listing of the tests' labels."""
    listing = subprocess.run(["ctest", "--test-dir", str(BUILD), "--show-only=json-v1"],
                             stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, check=True).stdout
    result = {}
    for test in json.loads(listing)["tests"]:
        labels = next((p["value"] for p in test.get("properties", []) if p["name"] == "LABELS"), [])
        for label in labels:
            if label.startswith("exe:"):
                result.setdefault(label[len("exe:"):], []).append(test["name"])
    return result


def all_skipped(ctest_log, by_program):
    """The programs every one of whose tests was skipped in `ctest_log`: they passed, but tested
    nothing, so they aren't recorded as passed."""
    skipped = set(re.findall(r"Test +#\d+: (\S+) \.+\s*\*+Skipped", ctest_log))
    return {name for name, tests in by_program.items() if tests and set(tests) <= skipped}


def check_registration(everything):
    """Every CTest test must carry the exe: and tier labels of a program in the manifest, as
    fillscope_test_program gives them; a test registered any other way would be invisible to the
    selection and to run-tests.sh's checks. Returns the problems."""
    tiers = {p.name: p.tier for p in everything}
    listing = subprocess.run(["ctest", "--test-dir", str(BUILD), "--show-only=json-v1"],
                             stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, check=True).stdout
    problems = []
    for test in json.loads(listing)["tests"]:
        labels = next((p["value"] for p in test.get("properties", []) if p["name"] == "LABELS"), [])
        owners = [label[len("exe:"):] for label in labels if label.startswith("exe:")]
        if len(owners) != 1 or owners[0] not in tiers:
            problems.append(f"{test['name']}: not registered through fillscope_test_program (labels {labels})")
        elif tiers[owners[0]] not in labels:
            problems.append(f"{test['name']}: lacks its program's tier label '{tiers[owners[0]]}'")
    return problems


def ctest(args):
    """Runs CTest in parallel, prints its output and the five slowest tests; returns (success, output)."""
    jobs = len(os.sched_getaffinity(0))  # the CPUs this process may use, as nproc counts them
    command = ["ctest", "--test-dir", str(BUILD), "-j", str(jobs), "--no-tests=error", "--output-on-failure", *args]
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    print(result.stdout, end="")
    times = re.findall(r"Test +#\d+: (\S+) \.+ +\S+ +([\d.]+) sec", result.stdout)
    slowest = sorted(times, key=lambda t: float(t[1]), reverse=True)[:5]
    if slowest:
        print("slowest: " + ", ".join(f"{name} {seconds}s" for name, seconds in slowest))
    return result.returncode == 0, result.stdout


def exe_labels(names):
    return ["-L", "^exe:(" + "|".join(sorted(names)) + ")$"]


def main(args):
    if args == ["--record-all"]:
        record(fingerprints(programs()))
        return 0
    if len(args) == 2 and args[0] == "--snapshot":
        write_atomically(pathlib.Path(args[1]), json.dumps(fingerprints(programs()), indent=1, sort_keys=True) + "\n")
        return 0
    if len(args) == 3 and args[0] == "--record-snapshot":
        snapshot = json.loads(pathlib.Path(args[1]).read_text())
        skipped = all_skipped(pathlib.Path(args[2]).read_text(), tests_by_program())
        for name in sorted(skipped):
            print(f"not recorded as passed, since every test was skipped: {name}")
        record({name: value for name, value in snapshot.items() if name not in skipped})
        return 0
    if args == ["--check-registration"]:
        problems = check_registration(programs())
        for problem in problems:
            print(f"PROBLEM: {problem}")
        return 1 if problems else 0
    if args not in ([], ["--all"], ["--list"]) and not (len(args) == 2 and args[0] == "-R"):
        print(USAGE, file=sys.stderr)
        return 2

    if not build():
        print("the build failed, so no tests were run")
        return 1
    everything = programs()
    fast = [p for p in everything if p.fast]
    state = load_state()
    now = fingerprints(everything)
    changed = {p.name for p in everything if state.get(p.name) != now[p.name]}

    if args == ["--list"]:
        for p in everything:
            status = "affected " if p.name in changed else "always   " if p.always else "unchanged"
            print(f"{status} {p.tier:9} {p.name}")
        return 0

    if args and args[0] == "-R":  # a filter: run what it names, record nothing
        return 0 if ctest([*args, *exe_labels(p.name for p in fast)])[0] else 1

    selected = {p.name for p in fast if args == ["--all"] or p.name in changed or p.always}
    slow_changed = sorted(p.name for p in everything if not p.fast and p.name in changed)
    if slow_changed:
        print("affected, slow tier (run tests/scripts/run-tests.sh): " + ", ".join(slow_changed))
    if not selected:
        print("no fast test program affected since its last passing run")
        return 0
    print("running: " + ", ".join(sorted(selected)))
    passed, output = ctest(exe_labels(selected))
    if not passed:
        return 1
    skipped = all_skipped(output, tests_by_program())
    record({name: now[name] for name in selected - skipped})
    return 0


sys.exit(main(sys.argv[1:]))
