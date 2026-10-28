# Checks that iterate.py selects the right test programs: changes one file at a time and compares
# the programs it reports affected with the ones that should be. A wrong selection would be silent
# (a test that should have run doesn't), which is why this check exists. It also runs iterate.py
# for a few changes, and checks what it ran and what it then recorded: a run that passed records
# its programs, one with a failing test records none of them, and the slow tier is left for
# run-tests.sh. And it checks the mutation check's choice of mutants the same way: with every
# mutant recorded as caught, then one file changed, mutants.py must choose exactly the mutants
# whose programs or file the change reaches, and a mutant no longer recorded must be chosen alone.
#
#   python3 -I tests/scripts/selection_check.py
#
# It works in a scratch copy of the working tree with a build and a pass record of its own
# (scratch_tree.py), so the files being edited, the build in use and the real pass record are never
# touched, whatever stops it.
import importlib.util
import json
import os
import pathlib
import re
import signal
import sys
import tempfile

# Loading the scripts beside this one by path would otherwise leave __pycache__ files in the tree.
sys.dont_write_bytecode = True

EXTENSION = {"vscode_debug_state_tests", "vscode_debug_state_typecheck"}
ATTRIBUTION = {"attribution_tests", "attribution_report_check", "attribution_copy_check"}
CODE_MAP = {"code_map_tests", "code_map_check"}
ANSWER = {"answer_tests", "answer_system_tests", "course_system_tests"}
# Answers' demo: the viewer's drawing and loop compiled in, its core linked, and the answer tool
# run, which runs exchange_replay.
DEMO = {"viewer_demo", "viewer_live_demo", "viewer_time_shift_demo", "viewer_slicing_demo"}
# The viewer program itself, run by its own test: its window and loop, its core, and the exchange.
VIEWER_PROGRAM = {"viewer_program_tests"}
ALL = {"common_tests", "docs_tests",
       "viewer_tests", "viewer_log_tests", "viewer_ui_tests", "viewer_replay_ui_tests", "scripts_tests", "backend_system_tests", "lowlatency_tests",
       "lowlatency_allocation_tests", "feed_tests", "time_shift_tests", "kept_file_tests",
       "feed_system_tests", "replay_debugger_tests", "session_system_tests", "truth_tests", "score_tests"} | EXTENSION | ATTRIBUTION | CODE_MAP | ANSWER | DEMO | VIEWER_PROGRAM
# The programs iterate.py runs every time, affected or not.
ALWAYS = {"docs_tests", "attribution_report_check", "code_map_check"}

# A change that reaches the binary: an extra string in the .comment section. (A comment in the
# source would change nothing the build produces, which is the point of building from the output.)
CPP_PROBE = '\nasm(".pushsection .comment\\n.asciz \\"fillscope selection probe\\"\\n.popsection");\n'
TEXT_PROBE = "\n# selection probe\n"
JS_PROBE = "\n// selection probe\n"
DELETE = None  # the file is removed rather than changed

def ui_timeout_changed(text):
    """A change to how one tier's tests run: the ui tier's timeout."""
    return text.replace("set(FILLSCOPE_TIMEOUT_ui 10)", "set(FILLSCOPE_TIMEOUT_ui 11)", 1)


# (file changed, probe, the programs that must be reported affected, and no others)
CASES = [
    ("src/viewer/viewer_view.cpp", CPP_PROBE, {"viewer_ui_tests", "viewer_replay_ui_tests"} | DEMO | VIEWER_PROGRAM),
    # The loop main.cpp runs is in ViewerApp, which the UI tests and the demos run too.
    ("src/viewer/viewer_app.cpp", CPP_PROBE, {"viewer_ui_tests", "viewer_replay_ui_tests"} | DEMO | VIEWER_PROGRAM),
    # The feed's socket, the atomic write and the answer tool's replay own their descriptors with
    # Fd, so everything that links them is affected, and the helpers' own tests.
    ("src/common/fd.h", CPP_PROBE,
     {"common_tests", "viewer_tests", "viewer_log_tests", "viewer_ui_tests", "viewer_replay_ui_tests", "feed_tests", "kept_file_tests", "feed_system_tests",
      "session_system_tests"}
     | ANSWER
     | DEMO | VIEWER_PROGRAM),
    # The viewer's core is linked by the viewer, its test programs and its demos, and by nothing else.
    ("src/viewer/context.cpp", CPP_PROBE, {"viewer_tests", "viewer_ui_tests", "viewer_replay_ui_tests"} | DEMO | VIEWER_PROGRAM),
    ("src/viewer/viewer_state.cpp", CPP_PROBE, {"viewer_tests", "viewer_ui_tests", "viewer_replay_ui_tests"} | DEMO | VIEWER_PROGRAM),
    ("src/viewer/session_truth.cpp", CPP_PROBE, {"viewer_tests", "viewer_ui_tests", "viewer_replay_ui_tests"} | DEMO | VIEWER_PROGRAM),
    # A replay's truth: read by its own tests, the system test that checks a session's, the scorer,
    # and the viewer, which shows a session's (and its tests and demos).
    ("src/truth/truth.cpp", CPP_PROBE,
     {"truth_tests", "session_system_tests", "score_tests", "viewer_tests", "viewer_ui_tests", "viewer_replay_ui_tests"} | DEMO | VIEWER_PROGRAM),
    # The detector's check: its own tests, and the system test that checks a real session's targets.
    ("src/score/score.cpp", CPP_PROBE, {"score_tests", "session_system_tests"}),
    # The simulated exchange: the tests of what is kept, and the UI test of a long replay.
    ("tests/feed/simulated_exchange.h", CPP_PROBE, {"time_shift_tests", "kept_file_tests", "viewer_ui_tests", "viewer_replay_ui_tests"}),
    # The books: Fillscope's reading of the market data, which the viewer (its log's events among
    # it), the answer tool and the system tests link.
    ("src/feed/book_builder.cpp", CPP_PROBE,
     {"feed_tests", "time_shift_tests", "kept_file_tests", "feed_system_tests", "viewer_tests", "viewer_log_tests",
      "viewer_ui_tests", "viewer_replay_ui_tests", "session_system_tests"}
     | ANSWER | DEMO | VIEWER_PROGRAM),
    ("tests/support/test_support.h", CPP_PROBE,
     {"feed_tests", "feed_system_tests", "viewer_ui_tests", "viewer_replay_ui_tests", "replay_debugger_tests", "answer_system_tests",
      "course_system_tests", "session_system_tests", "truth_tests", "score_tests", "viewer_tests"} | DEMO),
    ("tools/vscode-debug-state/src/debug_state.js", JS_PROBE, EXTENSION),
    ("tests/vscode-debug-state/debug_state.test.js", JS_PROBE, EXTENSION),
    ("docs/PLAN.md", TEXT_PROBE, {"docs_tests"}),
    ("tests/backend/replay_lldb_driver.py", TEXT_PROBE, {"replay_debugger_tests"}),
    # A run-time file deleted is a change too.
    ("tests/backend/replay_lldb_driver.py", DELETE, {"replay_debugger_tests"}),
    # Reaches its programs only as a run-time file: the replay binary they start.
    ("backend/Chapter12/exchange/exchange_replay.cpp", CPP_PROBE,
     {"feed_system_tests", "replay_debugger_tests", "answer_system_tests", "course_system_tests", "viewer_demo",
      "session_system_tests", "viewer_slicing_demo"}),
    ("tools/vscode-debug-state/tsconfig.json", JS_PROBE, EXTENSION),
    ("tools/attribution/attribution.py", TEXT_PROBE, ATTRIBUTION),
    ("tools/code_map/code_map.py", TEXT_PROBE, CODE_MAP),
    # The back end: its own tests link it (the allocation tests through their shared support, which
    # makes a matching engine), and the system test starts its programs.
    ("backend/Chapter12/exchange/matcher/matching_engine.cpp", CPP_PROBE,
     {"backend_system_tests", "lowlatency_tests", "lowlatency_allocation_tests", "feed_system_tests",
      "replay_debugger_tests", "answer_system_tests", "course_system_tests", "session_system_tests"} | DEMO | VIEWER_PROGRAM),
    # A strategy of the client's: in its own tests' program (the client's book is tested there), and
    # in trading_main, which every test that runs a client starts: not the lesson's demo, which plays
    # an answer, with no client.
    ("backend/Chapter12/trading/strategy/market_maker.h", CPP_PROBE,
     {"lowlatency_tests", "backend_system_tests", "feed_system_tests", "session_system_tests", "viewer_live_demo",
      "viewer_time_shift_demo", "viewer_slicing_demo"} | VIEWER_PROGRAM),
    # The slicer: in the fork's tests' program, and in trading_main, as the other strategies are.
    ("backend/Chapter12/trading/strategy/slicer.cpp", CPP_PROBE,
     {"lowlatency_tests", "backend_system_tests", "feed_system_tests", "session_system_tests", "viewer_live_demo",
      "viewer_time_shift_demo", "viewer_slicing_demo"} | VIEWER_PROGRAM),
    # The script that records a session: run only by the test of one.
    ("tools/sessions/record_session.py", TEXT_PROBE, {"session_system_tests", "viewer_slicing_demo"}),
    # The allocation tests are a program of their own, so a change to them affects only it.
    ("backend/Chapter12/tests/allocation_test.cpp", CPP_PROBE, {"lowlatency_allocation_tests"}),
    # A build file counts only through what it decides: what is built, and how CTest runs each
    # test. A comment decides neither, so it affects nothing; a tier's timeout changes how that
    # tier's tests run, so it affects that tier's programs and no others.
    ("CMakeLists.txt", TEXT_PROBE, set()),
    ("tests/viewer/CMakeLists.txt", TEXT_PROBE, set()),
    ("tests/CMakeLists.txt", ui_timeout_changed, {"viewer_ui_tests", "viewer_replay_ui_tests"}),
    # The viewer's window and loop: the viewer program's own test, and nothing else.
    ("src/viewer/main.cpp", CPP_PROBE, VIEWER_PROGRAM),
    # A file no program lists. docs_tests may still link to it, which is why it always runs (below);
    # the attribution check always runs because the history its report describes is no file, and
    # the code map's because which files the repository has is no file either.
    ("LICENSE", TEXT_PROBE, set()),
]

# (file changed, probe, the programs iterate.py must run): what it runs is what --list reports,
# plus those that always run; afterwards none of them may be affected. A change that affects only
# the slow tier runs only those that always run.
RUN_CASES = [
    ("LICENSE", TEXT_PROBE, ALWAYS),
    ("src/viewer/context.cpp", CPP_PROBE, ALWAYS | {"viewer_tests", "viewer_ui_tests", "viewer_replay_ui_tests"}),
    ("tests/backend/replay_lldb_driver.py", TEXT_PROBE, ALWAYS),
]

# The mutation check's choice of mutants (mutants.py): (file changed, probe, the programs whose
# change counts, or ALL): with everything recorded as caught, then `file` changed, it must choose
# exactly the mutants that run one of those programs or mutate `file`, no more and no fewer. Each
# programs set is the one CASES above checks iterate.py reports for that change.
ALL = "every mutant"
def unique_queue_mutant(text):
    """A change to one mutant's definition alone: its replacement gains a space."""
    return text.replace('[("ASSERT(num_elements_ != store_.size(),", "ASSERT(true,")]',
                        '[("ASSERT(num_elements_ != store_.size(),", "ASSERT(true ,")]', 1)
MUTANT_CASES = [
    ("LICENSE", TEXT_PROBE, set()),
    # A build file's comment changes nothing a mutant's result depends on.
    ("CMakeLists.txt", TEXT_PROBE, set()),
    ("src/viewer/viewer_view.cpp", CPP_PROBE, {"viewer_ui_tests", "viewer_replay_ui_tests"} | DEMO | VIEWER_PROGRAM),
    ("backend/Chapter12/exchange/matcher/matching_engine.cpp", CPP_PROBE,
     {"backend_system_tests", "lowlatency_tests", "lowlatency_allocation_tests", "feed_system_tests",
      "replay_debugger_tests", "answer_system_tests", "course_system_tests", "session_system_tests"} | DEMO | VIEWER_PROGRAM),
    # A test changed: every mutant its program's tests may catch runs again.
    ("backend/Chapter12/tests/lf_queue_test.cpp", CPP_PROBE, {"lowlatency_tests"}),
    # The check's own code: every mutant. (scripts_tests, whose run-time files it is, changes too.)
    ("tests/scripts/mutants.py", TEXT_PROBE, ALL),
    # One mutant's definition: that mutant, and those of scripts_tests, which reads mutants.py.
    ("tests/scripts/mutants.py", unique_queue_mutant, {"scripts_tests"}),
]
# The mutant whose definition unique_queue_mutant changes.
QUEUE_MUTANT = "a full queue is overwritten"

# A test made to fail: its program must not be recorded as passed, so it stays affected.
FAILING_TEST = "\nTEST(SelectionProbe, Fails) { FAIL() << \"selection_check.py made this fail\"; }\n"


def load_scratch_tree():
    """scratch_tree.py, beside this script. Loaded by path: python3 -I puts no script directory on
    the import path."""
    spec = importlib.util.spec_from_file_location("scratch_tree", pathlib.Path(__file__).with_name("scratch_tree.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


SCRATCH_TREE = load_scratch_tree()


class Copy:
    """The scratch copy and how to run its iterate.py."""

    def __init__(self, root, build, state):
        self.root = root
        self.env = {**os.environ, "FILLSCOPE_ITERATE_BUILD": str(build), "FILLSCOPE_ITERATE_STATE": str(state)}

    def iterate(self, *args, may_fail=False):
        result = SCRATCH_TREE.run([sys.executable, "-I", str(self.root / "tests" / "scripts" / "iterate.py"), *args],
                                  env=self.env)
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
        elif callable(self.probe):
            changed = self.probe(self.original)
            if changed == self.original:
                raise RuntimeError(f"the probe for {self.path} changed nothing")
            self.path.write_text(changed)
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
    """A run with a failing test records nothing (iterate.py records only a run that passed), so
    the failing test's program is still affected afterwards."""
    copy.iterate("--list")
    copy.iterate("--record-all")
    with copy.changed("tests/viewer/viewer_test.cpp", FAILING_TEST):
        output = copy.iterate(may_fail=True)
        still = copy.affected()
    ran = ran_in(output)
    ok = "viewer_tests" in ran and still == {"viewer_tests"}
    print(f"{'ok  ' if ok else 'FAIL'} run, a failing test: still affected afterwards: {', '.join(sorted(still)) or 'none'}")
    return None if ok else f"run, a failing test: expected only viewer_tests still affected, got {sorted(still)}"


class Mutants:
    """The copy's mutation check, with a copy and record of its own in `directory`, and the
    copy's mutants, each with the programs it runs as this check works them out: from CTest's
    listing of every test, matched here (the mutation check asks CTest about each name instead)."""

    def __init__(self, copy, build, directory):
        self.copy = copy
        self.directory = directory
        self.env = {**os.environ, "FILLSCOPE_MUTANTS_DIR": str(directory)}
        spec = importlib.util.spec_from_file_location("copy_mutants", copy.root / "tests" / "scripts" / "mutants.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        # As root (CI's container), the check leaves out the mutants it can't arrange (it says so).
        skipped = module.NEEDS_A_USER_NOT_ROOT if os.geteuid() == 0 else set()
        self.mutants = [(name, file, tests) for name, file, _, tests in module.MUTANTS if name not in skipped]
        listing = json.loads(SCRATCH_TREE.run(["ctest", "--test-dir", str(build), "--show-only=json-v1"]).stdout)
        self.tests = {t["name"]: {label[len("exe:"):] for p in t.get("properties", []) if p["name"] == "LABELS"
                                  for label in p["value"] if label.startswith("exe:")}
                      for t in listing["tests"]}

    def run(self, *args):
        result = SCRATCH_TREE.run([sys.executable, "-I", str(self.copy.root / "tests" / "scripts" / "mutants.py"), *args],
                                  env=self.env)
        if result.returncode != 0:
            raise RuntimeError(f"mutants.py {' '.join(args)} failed:\n{result.stdout[-3000:]}")
        return result.stdout

    def chosen(self):
        return {line[len("run   "):] for line in self.run("--list").splitlines() if line.startswith("run   ")}

    def programs(self, tests):
        found = set()
        for part in tests.split(" + "):
            part = part.strip()
            prefix = part.split(":", 1)[0]
            if prefix in ("ui", "node", "lowlatency"):
                found.add({"ui": "viewer_ui_tests", "node": "vscode_debug_state_tests",
                           "lowlatency": "lowlatency_tests"}[prefix])
            else:
                found |= {p for name, programs in self.tests.items() if re.search(part, name) for p in programs}
        return found

    def expected(self, file, programs):
        if programs == ALL:
            return {name for name, _, _ in self.mutants}
        return {name for name, mutated, tests in self.mutants if mutated == file or self.programs(tests) & programs}


def mutant_case(mutants, file, probe, programs):
    """mutants.py with everything recorded as caught, then `file` changed: returns a problem, or None."""
    mutants.run("--record-all")
    with mutants.copy.changed(file, probe):
        chosen = mutants.chosen()
    expected = mutants.expected(file, programs)
    if probe is not TEXT_PROBE and probe is not CPP_PROBE and probe is not DELETE:
        expected.add(QUEUE_MUTANT)
    what = getattr(probe, "__name__", "changed")
    ok = chosen == expected
    print(f"{'ok  ' if ok else 'FAIL'} mutants, {file} {what}: {len(chosen)} chosen of {len(mutants.mutants)}")
    if ok:
        return None
    return (f"mutants, {file} {what}: chose {len(chosen)}, expected {len(expected)}; "
            f"extra {sorted(chosen - expected)[:5]}, missing {sorted(expected - chosen)[:5]}")


def mutant_not_caught_case(mutants):
    """A mutant whose record is gone, as when it wasn't caught, is chosen alone."""
    mutants.run("--record-all")
    record_path = mutants.directory / "last-caught.json"
    record = json.loads(record_path.read_text())
    del record[QUEUE_MUTANT]
    record_path.write_text(json.dumps(record))
    chosen = mutants.chosen()
    ok = chosen == {QUEUE_MUTANT}
    print(f"{'ok  ' if ok else 'FAIL'} mutants, one not caught last time: chose {', '.join(sorted(chosen)) or 'none'}")
    return None if ok else f"mutants, one not caught last time: chose {sorted(chosen)}"


def main():
    for signum in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(signum, stop_on_signal)
    scratch_tree = SCRATCH_TREE
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
            mutants = Mutants(copy, build, pathlib.Path(scratch) / "mutants")
            for file, probe, programs in MUTANT_CASES:
                problem = mutant_case(mutants, file, probe, programs)
                if problem:
                    failures.append(problem)
            problem = mutant_not_caught_case(mutants)
            if problem:
                failures.append(problem)
    finally:
        scratch_tree.remove(copy_root)
    for failure in failures:
        print(f"PROBLEM: {failure}")
    total = len(CASES) + len(RUN_CASES) + 1 + len(MUTANT_CASES) + 1
    print(f"{total - len(failures)} of {total} selections right")
    return 0 if not failures else 1


sys.exit(main())
