# Checks that iterate.py selects the right test programs: changes one file at a time and compares
# the programs it reports affected with the ones that should be. A wrong selection would be silent
# (a test that should have run doesn't), which is why this check exists.
#
#   python3 -I tests/scripts/selection_check.py
#
# It also runs iterate.py for a few changes, and checks what it ran and what it then recorded.
#
# Uses its own pass record, never the real one; the source is always restored, even on Ctrl-C,
# SIGTERM or a closed terminal.
import os
import pathlib
import signal
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
EXTENSION = {"vscode_debug_state_tests", "vscode_debug_state_typecheck"}
ALL = {"channel_tests", "docs_tests", "lockstep_tests", "frame_source_tests", "lockstep_debugger_tests",
       "viewer_tests", "viewer_ui_tests"} | EXTENSION

# A change that reaches the binary: an extra string in the .comment section. (A comment in the
# source would change nothing the build produces, which is the point of building from the output.)
CPP_PROBE = '\nasm(".pushsection .comment\\n.asciz \\"fillscope selection probe\\"\\n.popsection");\n'
TEXT_PROBE = "\n# selection probe\n"
JS_PROBE = "\n// selection probe\n"

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
    ("tests/support/test_support.h", CPP_PROBE, ALL - {"lockstep_tests", "docs_tests", "viewer_tests"} - EXTENSION),
    ("tools/vscode-debug-state/src/debug_state.js", JS_PROBE, EXTENSION),
    ("tests/vscode-debug-state/debug_state.test.js", JS_PROBE, EXTENSION),
    ("docs/PLAN.md", TEXT_PROBE, {"docs_tests"}),
    ("tests/lockstep/lldb_driver.py", TEXT_PROBE, {"lockstep_debugger_tests"}),
    ("CMakeLists.txt", TEXT_PROBE, ALL),
    # Any build file can change how a test runs (a timeout, a label), so each one counts for all.
    ("tests/viewer/CMakeLists.txt", TEXT_PROBE, ALL),
    # Nothing tests the viewer's window yet, so changing it affects no test program.
    ("src/viewer/main.cpp", CPP_PROBE, set()),
    # A file no program lists. docs_tests may still link to it, which is why it always runs (below).
    ("LICENSE", TEXT_PROBE, set()),
]

# (file changed, probe, the programs iterate.py must run): what it runs is what --list reports,
# plus docs_tests, which always runs; afterwards none of them may be affected.
RUN_CASES = [
    ("LICENSE", TEXT_PROBE, {"docs_tests"}),
    ("src/viewer/context.cpp", CPP_PROBE, {"docs_tests", "viewer_tests", "viewer_ui_tests"}),
]


def iterate(*args, env):
    return subprocess.run([sys.executable, "-I", str(ROOT / "tests" / "scripts" / "iterate.py"), *args], env=env,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, check=True).stdout


def stop_on_signal(signum, _frame):
    # Raised in the main thread, so the `finally` that restores the source runs, as on Ctrl-C.
    raise SystemExit(128 + signum)


def run_case(file, probe, expected, env):
    """Runs iterate.py with `file` changed; returns a problem, or None."""
    iterate("--list", env=env)
    iterate("--record-all", env=env)
    path = ROOT / file
    original = path.read_text()
    try:
        path.write_text(original + probe)
        output = iterate(env=env)
        listing = iterate("--list", env=env)
    finally:
        path.write_text(original)
    ran = next((set(line[len("running: "):].split(", ")) for line in output.splitlines()
                if line.startswith("running: ")), set())
    still = {line.split()[-1] for line in listing.splitlines() if line.startswith("affected ")} & expected
    status = "ok  " if ran == expected and not still else "FAIL"
    print(f"{status} run, {file}: ran {', '.join(sorted(ran)) or 'none'}")
    if ran != expected:
        return f"run, {file}: expected to run {sorted(expected)}, ran {sorted(ran)}"
    if still:
        return f"run, {file}: passed but not recorded: {sorted(still)}"
    return None


def main():
    for signum in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(signum, stop_on_signal)
    failures = []
    with tempfile.TemporaryDirectory() as scratch:
        env = {**os.environ, "FILLSCOPE_ITERATE_STATE": str(pathlib.Path(scratch) / "state.json")}
        for file, probe, expected in CASES:
            iterate("--list", env=env)          # builds the real code
            iterate("--record-all", env=env)    # the baseline: nothing affected
            path = ROOT / file
            original = path.read_text()
            try:
                path.write_text(original + probe)
                listing = iterate("--list", env=env)
            finally:
                path.write_text(original)
            reported = {line.split()[-1] for line in listing.splitlines() if line.startswith("affected ")}
            status = "ok  " if reported == expected else "FAIL"
            print(f"{status} {file}: {', '.join(sorted(reported)) or 'none'}")
            if reported != expected:
                failures.append(f"{file}: expected {sorted(expected)}, got {sorted(reported)}")
        for file, probe, expected in RUN_CASES:
            problem = run_case(file, probe, expected, env)
            if problem:
                failures.append(problem)
        iterate("--list", env=env)  # leave the build matching the real code
    for failure in failures:
        print(f"PROBLEM: {failure}")
    total = len(CASES) + len(RUN_CASES)
    print(f"{total - len(failures)} of {total} selections right")
    return 0 if not failures else 1


sys.exit(main())
