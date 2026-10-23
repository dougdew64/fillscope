# Checks that iterate.py selects the right test programs: changes one file at a time and compares
# the programs it reports affected with the ones that should be. A wrong selection would be silent
# (a test that should have run doesn't), which is why this check exists. It also runs iterate.py
# for a few changes, and checks what it ran and what it then recorded: a program that passed is
# recorded, one that failed is not, and the slow tier is left for run-tests.sh.
#
#   python3 -I tests/scripts/selection_check.py
#
# It works in a scratch copy of the working tree with a build and a pass record of its own
# (scratch_tree.py), so the files being edited, the build in use and the real pass record are never
# touched, whatever stops it.
import importlib.util
import os
import pathlib
import signal
import subprocess
import sys
import tempfile

# Loading the scripts beside this one by path would otherwise leave __pycache__ files in the tree.
sys.dont_write_bytecode = True

EXTENSION = {"vscode_debug_state_tests", "vscode_debug_state_typecheck"}
ATTRIBUTION = {"attribution_tests", "attribution_report_check"}
ALL = {"channel_tests", "docs_tests", "lockstep_tests", "frame_source_tests", "lockstep_debugger_tests",
       "viewer_tests", "viewer_ui_tests", "scripts_tests", "backend_system_tests"} | EXTENSION | ATTRIBUTION
# The programs iterate.py runs every time, affected or not.
ALWAYS = {"docs_tests", "attribution_report_check"}

# A change that reaches the binary: an extra string in the .comment section. (A comment in the
# source would change nothing the build produces, which is the point of building from the output.)
CPP_PROBE = '\nasm(".pushsection .comment\\n.asciz \\"fillscope selection probe\\"\\n.popsection");\n'
TEXT_PROBE = "\n# selection probe\n"
JS_PROBE = "\n// selection probe\n"
DELETE = None  # the file is removed rather than changed

# (file changed, probe, the programs that must be reported affected, and no others)
CASES = [
    ("src/viewer/viewer_view.cpp", CPP_PROBE, {"viewer_ui_tests"}),
    # The loop main.cpp runs is in ViewerApp, which the UI tests run too.
    ("src/viewer/viewer_app.cpp", CPP_PROBE, {"viewer_ui_tests"}),
    ("src/channel/frame.h", CPP_PROBE,
     {"channel_tests", "frame_source_tests", "lockstep_debugger_tests", "viewer_tests", "viewer_ui_tests"}),
    # The viewer's core is linked by both of the viewer's test programs, and by nothing else.
    ("src/viewer/context.cpp", CPP_PROBE, {"viewer_tests", "viewer_ui_tests"}),
    ("src/lockstep/anchor.cpp", CPP_PROBE, {"lockstep_tests", "frame_source_tests", "lockstep_debugger_tests", "viewer_ui_tests"}),
    ("tools/frame_source/frame_source.cpp", CPP_PROBE, {"frame_source_tests", "lockstep_debugger_tests", "viewer_ui_tests"}),
    ("tests/support/test_support.h", CPP_PROBE, ALL - {"lockstep_tests", "docs_tests", "viewer_tests", "scripts_tests", "backend_system_tests"} - EXTENSION - ATTRIBUTION),
    ("tools/vscode-debug-state/src/debug_state.js", JS_PROBE, EXTENSION),
    ("tests/vscode-debug-state/debug_state.test.js", JS_PROBE, EXTENSION),
    ("docs/PLAN.md", TEXT_PROBE, {"docs_tests"}),
    ("tests/lockstep/lldb_driver.py", TEXT_PROBE, {"lockstep_debugger_tests"}),
    # A run-time file deleted is a change too.
    ("tests/lockstep/lldb_driver.py", DELETE, {"lockstep_debugger_tests"}),
    # Reaches its programs only as a run-time file: the frame source binary they start.
    ("tools/frame_source/main.cpp", CPP_PROBE, {"frame_source_tests", "lockstep_debugger_tests", "viewer_ui_tests"}),
    ("src/channel/net.cpp", CPP_PROBE,
     {"channel_tests", "frame_source_tests", "lockstep_debugger_tests", "viewer_tests", "viewer_ui_tests"}),
    ("tools/vscode-debug-state/tsconfig.json", JS_PROBE, EXTENSION),
    ("tools/attribution/attribution.py", TEXT_PROBE, ATTRIBUTION),
    # The back end reaches its test only as run-time files: the exchange and the client.
    ("backend/Chapter12/exchange/matcher/matching_engine.cpp", CPP_PROBE, {"backend_system_tests"}),
    ("CMakeLists.txt", TEXT_PROBE, ALL),
    # Any build file can change how a test runs (a timeout, a label), so each one counts for all.
    ("tests/viewer/CMakeLists.txt", TEXT_PROBE, ALL),
    # Nothing tests the viewer's window yet, so changing it affects no test program.
    ("src/viewer/main.cpp", CPP_PROBE, set()),
    # A file no program lists. docs_tests may still link to it, which is why it always runs (below);
    # the attribution check always runs because the history its report describes is no file.
    ("LICENSE", TEXT_PROBE, set()),
]

# (file changed, probe, the programs iterate.py must run): what it runs is what --list reports,
# plus those that always run; afterwards none of them may be affected. A change that affects only
# the slow tier runs only those that always run.
RUN_CASES = [
    ("LICENSE", TEXT_PROBE, ALWAYS),
    ("src/viewer/context.cpp", CPP_PROBE, ALWAYS | {"viewer_tests", "viewer_ui_tests"}),
    ("tests/lockstep/lldb_driver.py", TEXT_PROBE, ALWAYS),
]

# A test made to fail: its program must not be recorded as passed, so it stays affected.
FAILING_TEST = "\nTEST(SelectionProbe, Fails) { FAIL() << \"selection_check.py made this fail\"; }\n"


def load_scratch_tree():
    """scratch_tree.py, beside this script. Loaded by path: python3 -I puts no script directory on
    the import path."""
    spec = importlib.util.spec_from_file_location("scratch_tree", pathlib.Path(__file__).with_name("scratch_tree.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Copy:
    """The scratch copy and how to run its iterate.py."""

    def __init__(self, root, build, state):
        self.root = root
        self.env = {**os.environ, "FILLSCOPE_ITERATE_BUILD": str(build), "FILLSCOPE_ITERATE_STATE": str(state)}

    def iterate(self, *args, may_fail=False):
        result = subprocess.run([sys.executable, "-I", str(self.root / "tests" / "scripts" / "iterate.py"), *args],
                                env=self.env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if result.returncode != 0 and not may_fail:
            raise RuntimeError(f"iterate.py {' '.join(args)} failed:\n{result.stdout[-3000:]}")
        return result.stdout

    def affected(self):
        return {line.split()[-1] for line in self.iterate("--list").splitlines() if line.startswith("affected ")}

    def changed(self, file, probe):
        """A context in which `file` is changed by `probe` (or removed, for DELETE)."""
        return Changed(self.root / file, probe)


class Changed:
    def __init__(self, path, probe):
        self.path, self.probe = path, probe

    def __enter__(self):
        self.original = self.path.read_text()
        if self.probe is DELETE:
            self.path.unlink()
        else:
            self.path.write_text(self.original + self.probe)

    def __exit__(self, *_):
        self.path.write_text(self.original)


def stop_on_signal(signum, _frame):
    # Raised in the main thread, so the `finally` that removes the copy runs, as on Ctrl-C.
    raise SystemExit(128 + signum)


def ran_in(output):
    return next((set(line[len("running: "):].split(", ")) for line in output.splitlines()
                 if line.startswith("running: ")), set())


def run_case(copy, file, probe, expected):
    """Runs iterate.py with `file` changed; returns a problem, or None."""
    copy.iterate("--list")
    copy.iterate("--record-all")
    with copy.changed(file, probe):
        ran = ran_in(copy.iterate())
        still = copy.affected() & expected
    status = "ok  " if ran == expected and not still else "FAIL"
    print(f"{status} run, {file}: ran {', '.join(sorted(ran)) or 'none'}")
    if ran != expected:
        return f"run, {file}: expected to run {sorted(expected)}, ran {sorted(ran)}"
    if still:
        return f"run, {file}: passed but not recorded: {sorted(still)}"
    return None


def failing_case(copy):
    """A failing test's program is not recorded as passed, though the rest of the run's are."""
    copy.iterate("--list")
    copy.iterate("--record-all")
    with copy.changed("tests/viewer/viewer_test.cpp", FAILING_TEST):
        output = copy.iterate(may_fail=True)
        still = copy.affected()
    ran = ran_in(output)
    ok = "viewer_tests" in ran and still == {"viewer_tests"}
    print(f"{'ok  ' if ok else 'FAIL'} run, a failing test: still affected afterwards: {', '.join(sorted(still)) or 'none'}")
    return None if ok else f"run, a failing test: expected only viewer_tests still affected, got {sorted(still)}"


def main():
    for signum in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(signum, stop_on_signal)
    scratch_tree = load_scratch_tree()
    root = pathlib.Path(__file__).resolve().parents[2]
    copy_root = scratch_tree.make(root)
    failures = []
    try:
        build = scratch_tree.configure(copy_root, root)
        if build is None:
            print("PROBLEM: the copy of the working tree couldn't be configured")
            return 1
        with tempfile.TemporaryDirectory() as scratch:
            copy = Copy(copy_root, build, pathlib.Path(scratch) / "state.json")
            for file, probe, expected in CASES:
                copy.iterate("--list")          # builds the unchanged code
                copy.iterate("--record-all")    # the baseline: nothing affected
                with copy.changed(file, probe):
                    reported = copy.affected()
                change = "removed" if probe is DELETE else "changed"
                status = "ok  " if reported == expected else "FAIL"
                print(f"{status} {file} {change}: {', '.join(sorted(reported)) or 'none'}")
                if reported != expected:
                    failures.append(f"{file} {change}: expected {sorted(expected)}, got {sorted(reported)}")
            for file, probe, expected in RUN_CASES:
                problem = run_case(copy, file, probe, expected)
                if problem:
                    failures.append(problem)
            problem = failing_case(copy)
            if problem:
                failures.append(problem)
    finally:
        scratch_tree.remove(copy_root)
    for failure in failures:
        print(f"PROBLEM: {failure}")
    total = len(CASES) + len(RUN_CASES) + 1
    print(f"{total - len(failures)} of {total} selections right")
    return 0 if not failures else 1


sys.exit(main())
