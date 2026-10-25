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
#   --check-registration                every CTest test belongs to a program in the manifest, and
#                                       every program has a test
#   --check-sanitizers yes|no           every program in the build (its manifest's, the viewer,
#                                       and the back end's exchange, client and replay) is linked
#                                       with both sanitizers (yes), or with neither (no)
#
# FILLSCOPE_ITERATE_BUILD and FILLSCOPE_ITERATE_STATE choose another build directory and pass
# record; the other FILLSCOPE_ variables configure the tests, so they count in the fingerprint
# (all but FILLSCOPE_WERROR, which matters only when the build is configured, and so already shows
# in what the build produces).
#
# "Affected" comes from the build, not from a hand-kept map: each test program, and each file it
# uses at run time, is listed in build/debug/test-manifest.txt (written by tests/CMakeLists.txt). A
# program is affected when its binary, one of those files (for the extension's tests, the Node.js
# that runs them among them), a shared library it loads, a FILLSCOPE_ environment variable, or any
# CMakeLists.txt differs from its last passing run. A change the build doesn't propagate into a
# program can't change its result.
#
# A program marked "always" in the manifest runs on every iteration: its result depends on files
# it can't list (docs_tests follows links to any file in the repository), and it is fast.
#
# Each program's fingerprint is taken before its tests run, and recorded only if they pass, and
# only for a program that has tests and didn't skip them all: a file edited while the tests ran is
# then seen as changed next time, rather than recorded as tested.
#
# While it builds and runs tests, it holds a lock on the build directory, so two runs (this and
# run-tests.sh, or this from a terminal and from the editor) never build or test it at once.
import fcntl
import hashlib
import json
import os
import pathlib
import re
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
# A file at least this large is fingerprinted by its size and modification time rather than its
# contents (Node.js's binary is about 100 MB), which costs nothing and still sees it replaced.
LARGE_FILE = 8 * 1024 * 1024
# Environment variables that don't change a test's result once the build exists.
NOT_FINGERPRINTED = ("FILLSCOPE_ITERATE_", "FILLSCOPE_WERROR")

USAGE = """usage: iterate.py [--all | --list | -R REGEX]
       iterate.py --snapshot FILE | --record-snapshot FILE CTEST_LOG | --record-all
       iterate.py --check-registration | --check-sanitizers yes|no"""


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


def programs(manifest=None):
    text = (manifest if manifest is not None else MANIFEST.read_text())
    return [Program(line) for line in text.splitlines() if line.strip()]


def build_files():
    """Every CMakeLists.txt of ours and of the back end's, tracked or new: a change to any can change
    how a test is run. Asking git leaves out every build directory and installed package, whatever
    its name. The back end is a submodule, whose files the top level's listing doesn't include, so
    it is asked too (in a copy without the submodule's own repository, git answers for the copy)."""
    found = set()
    for directory, patterns in ((ROOT, ["CMakeLists.txt", "*/CMakeLists.txt"]),
                                (ROOT / "backend", ["Chapter12/CMakeLists.txt", "Chapter12/*/CMakeLists.txt"])):
        if not directory.is_dir():
            continue
        listed = subprocess.run(["git", "-C", str(directory), "ls-files", "--cached", "--others", "--exclude-standard",
                                 "-z", "--", *patterns], stdout=subprocess.PIPE, check=True).stdout.decode()
        found |= {str(directory / name) for name in listed.split("\0") if name and (directory / name).exists()}
    return sorted(found)


def environment():
    """The environment variables that configure our tests, as text."""
    return "\n".join(f"{key}={value}" for key, value in sorted(os.environ.items())
                     if key.startswith("FILLSCOPE_") and not key.startswith(NOT_FINGERPRINTED))


def stat_of(path):
    s = os.stat(path)
    return f"{path} {s.st_size} {s.st_mtime_ns}"


def file_digest(path, whole=False):
    """What a file's fingerprint takes from it: its contents' hash, its size and time if it is
    large and not `whole`, or that it is missing (a run-time file deleted is a change too, not an
    error). A test program's own binary is always hashed whole (about 20 ms for the largest), so a
    relink to the same bytes is no change, and different bytes always are."""
    file = pathlib.Path(path)
    if not file.exists():
        return b"missing"
    if not whole and file.stat().st_size >= LARGE_FILE:
        return stat_of(path).encode()
    return hashlib.sha256(file.read_bytes()).digest()


def shared_libraries(binary):
    """The shared libraries `binary` loads, by path, size and modification time: an updated system
    library changes the fingerprint without being hashed whole."""
    output = subprocess.run(["ldd", binary], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True).stdout
    paths = sorted(set(re.findall(r"=> (/\S+)", output)))
    return [stat_of(p) for p in paths if os.path.exists(p)]


def fingerprint(program, common):
    """`common` is what every program depends on: the build files and the environment."""
    digest = hashlib.sha256(common.encode())
    for path in program.files:
        digest.update(path.encode())
        digest.update(file_digest(path, whole=path == program.binary))
    if program.binary is not None and os.path.exists(program.binary):
        for line in shared_libraries(program.binary):
            digest.update(line.encode())
    return digest.hexdigest()


def fingerprints(selected):
    """{name: fingerprint} for each program in `selected`, as the files are now."""
    build_digest = hashlib.sha256()
    for path in build_files():
        build_digest.update(path.encode())
        build_digest.update(file_digest(path))
    common = build_digest.hexdigest() + environment()
    return {p.name: fingerprint(p, common) for p in selected}


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


def record(passed, known=None):
    """Records `passed` ({name: fingerprint}) as the latest passing runs, and drops the record of
    any program not in `known` (the names of the programs the build lists now), if given: a program
    since removed would otherwise stay in the record for good."""
    state = load_state()
    if known is not None:
        state = {name: value for name, value in state.items() if name in known}
    state.update(passed)
    write_atomically(STATE, json.dumps(state, indent=1, sort_keys=True) + "\n")


def ctest_listing():
    """CTest's own description of every test: name, command and labels."""
    listing = subprocess.run(["ctest", "--test-dir", str(BUILD), "--show-only=json-v1"],
                             stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, check=True).stdout
    return json.loads(listing)["tests"]


def labels_of(test):
    return next((p["value"] for p in test.get("properties", []) if p["name"] == "LABELS"), [])


def tests_by_program(listing):
    """{program name: [CTest test names]}, from the tests' exe: labels."""
    result = {}
    for test in listing:
        for label in labels_of(test):
            if label.startswith("exe:"):
                result.setdefault(label[len("exe:"):], []).append(test["name"])
    return result


def not_to_record(ctest_log, everything, by_program):
    """The programs that passed but tested nothing, so aren't recorded as passed: those with no
    tests at all, and those every one of whose tests was skipped in `ctest_log`."""
    skipped = set(re.findall(r"Test +#\d+: (\S+) \.+\s*\*+Skipped", ctest_log))
    return {p.name for p in everything if not by_program.get(p.name) or set(by_program[p.name]) <= skipped}


def check_registration(everything, listing):
    """Every CTest test must carry the exe: and tier labels of a program in the manifest, as
    fillscope_test_program gives them, and run that program (or, for a program that is a command,
    be the one test of that name); every program must have a test. A test registered any other way
    would be invisible to the selection and to run-tests.sh's checks, or recorded under a program
    whose fingerprint doesn't cover what it runs. Returns the problems."""
    by_name = {p.name: p for p in everything}
    problems = []
    for test in listing:
        labels = labels_of(test)
        owners = [label[len("exe:"):] for label in labels if label.startswith("exe:")]
        if len(owners) != 1 or owners[0] not in by_name:
            problems.append(f"{test['name']}: not registered through fillscope_test_program (labels {labels})")
            continue
        program = by_name[owners[0]]
        command = test.get("command", [])
        if program.tier not in labels:
            problems.append(f"{test['name']}: lacks its program's tier label '{program.tier}'")
        elif program.binary is not None and program.binary not in command:
            problems.append(f"{test['name']}: labelled {program.name}, but doesn't run it ({command})")
        elif program.binary is None and test["name"] != program.name:
            problems.append(f"{test['name']}: labelled {program.name}, a command registered as a test of that name")
    with_tests = tests_by_program(listing)
    for program in everything:
        if not with_tests.get(program.name):
            problems.append(f"{program.name}: has no tests, so it would pass having tested nothing")
    return problems


def sanitizer_problems(binaries, sanitized, ldd):
    """Whether each of `binaries` is linked with AddressSanitizer and UndefinedBehaviorSanitizer
    (`sanitized`), or with neither. `ldd(binary)` gives ldd's output. Returns the problems."""
    problems = []
    for binary in binaries:
        libraries = ldd(binary)
        for library in ("libasan", "libubsan"):
            linked = library in libraries
            if sanitized and not linked:
                problems.append(f"{binary} is in the sanitizer build but not linked with {library}")
            elif not sanitized and linked:
                problems.append(f"{binary} is in the normal build but linked with {library}")
    return problems


def run_ldd(binary):
    return subprocess.run(["ldd", binary], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True).stdout


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


def lock_build():
    """Holds an exclusive lock on the build directory until this process ends, waiting for any
    other run that holds it. run-tests.sh takes the same lock (flock on the same file)."""
    BUILD.mkdir(parents=True, exist_ok=True)
    lock = open(BUILD / ".fillscope-lock", "w")  # kept open: closing it would release the lock
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        print(f"waiting for another run using {BUILD}")
        fcntl.flock(lock, fcntl.LOCK_EX)
    return lock


def main(args):
    if args == ["--record-all"]:
        everything = programs()
        record(fingerprints(everything), known={p.name for p in everything})
        return 0
    if len(args) == 2 and args[0] == "--snapshot":
        write_atomically(pathlib.Path(args[1]), json.dumps(fingerprints(programs()), indent=1, sort_keys=True) + "\n")
        return 0
    if len(args) == 3 and args[0] == "--record-snapshot":
        snapshot = json.loads(pathlib.Path(args[1]).read_text())
        everything = programs()
        excluded = not_to_record(pathlib.Path(args[2]).read_text(), everything, tests_by_program(ctest_listing()))
        for name in sorted(excluded):
            print(f"not recorded as passed, since it tested nothing: {name}")
        record({name: value for name, value in snapshot.items() if name not in excluded},
               known={p.name for p in everything})
        return 0
    if len(args) == 2 and args[0] == "--check-sanitizers" and args[1] in ("yes", "no"):
        binaries = [p.binary for p in programs() if p.binary is not None] + [
            str(BUILD / "viewer"),
            *(str(BUILD / "backend" / "Chapter12" / program) for program in ("exchange_main", "trading_main", "exchange_replay"))]
        problems = sanitizer_problems(binaries, args[1] == "yes", run_ldd)
        for problem in problems:
            print(f"PROBLEM: {problem}")
        return 1 if problems else 0
    if args == ["--check-registration"]:
        problems = check_registration(programs(), ctest_listing())
        for problem in problems:
            print(f"PROBLEM: {problem}")
        return 1 if problems else 0
    if args not in ([], ["--all"], ["--list"]) and not (len(args) == 2 and args[0] == "-R"):
        print(USAGE, file=sys.stderr)
        return 2

    lock = lock_build()  # noqa: F841 (held, not used: released when the process ends)
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
    if not any(p.name in selected and not p.always for p in fast):
        print("no fast test program affected since its last passing run; running only those that always run")
    print("running: " + ", ".join(sorted(selected)))
    passed, output = ctest(exe_labels(selected))
    if not passed:
        return 1
    excluded = not_to_record(output, [p for p in everything if p.name in selected], tests_by_program(ctest_listing()))
    record({name: now[name] for name in selected - excluded}, known={p.name for p in everything})
    return 0


if __name__ == "__main__":  # imported by the scripts' own tests
    sys.exit(main(sys.argv[1:]))
