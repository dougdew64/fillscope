# Checks that iterate.py selects the right test programs: changes one file at a time and compares
# the programs it reports affected with the ones that should be. A wrong selection would be silent
# (a test that should have run doesn't), which is why this check exists.
#
#   python3 -I tests/scripts/selection_check.py
#
# Uses its own pass record, never the real one; the source is always restored.
import os
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
EXTENSION = {"vscode_debug_state_tests", "vscode_debug_state_typecheck"}
ALL = {"channel_tests", "docs_tests", "lockstep_tests", "frame_source_tests", "lockstep_debugger_tests",
       "viewer_ui_tests"} | EXTENSION

# A change that reaches the binary: an extra string in the .comment section. (A comment in the
# source would change nothing the build produces, which is the point of building from the output.)
CPP_PROBE = '\nasm(".pushsection .comment\\n.asciz \\"fillscope selection probe\\"\\n.popsection");\n'
TEXT_PROBE = "\n# selection probe\n"
JS_PROBE = "\n// selection probe\n"

# (file changed, probe, the programs that must be reported affected, and no others)
CASES = [
    ("src/viewer/viewer_view.cpp", CPP_PROBE, {"viewer_ui_tests"}),
    ("src/channel/frame.h", CPP_PROBE, {"channel_tests", "frame_source_tests", "lockstep_debugger_tests", "viewer_ui_tests"}),
    ("src/lockstep/anchor.cpp", CPP_PROBE, {"lockstep_tests", "frame_source_tests", "lockstep_debugger_tests", "viewer_ui_tests"}),
    ("tools/frame_source/frame_source.cpp", CPP_PROBE, {"frame_source_tests", "lockstep_debugger_tests", "viewer_ui_tests"}),
    ("tests/support/test_support.h", CPP_PROBE, ALL - {"lockstep_tests", "docs_tests"} - EXTENSION),
    ("tools/vscode-debug-state/src/debug_state.js", JS_PROBE, EXTENSION),
    ("tests/vscode-debug-state/debug_state.test.js", JS_PROBE, EXTENSION),
    ("docs/PLAN.md", TEXT_PROBE, {"docs_tests"}),
    ("tests/lockstep/lldb_self_stop.py", TEXT_PROBE, {"lockstep_debugger_tests"}),
    ("CMakeLists.txt", TEXT_PROBE, ALL),
    # Any build file can change how a test runs (a timeout, a label), so each one counts for all.
    ("tests/viewer/CMakeLists.txt", TEXT_PROBE, ALL),
    # Nothing tests the viewer's window yet, so changing it affects no test program.
    ("src/viewer/main.cpp", CPP_PROBE, set()),
]


def iterate(*args, env):
    return subprocess.run([sys.executable, "-I", str(ROOT / "tests" / "scripts" / "iterate.py"), *args], env=env,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, check=True).stdout


def main():
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
        iterate("--list", env=env)  # leave the build matching the real code
    for failure in failures:
        print(f"PROBLEM: {failure}")
    print(f"{len(CASES) - len(failures)} of {len(CASES)} selections right")
    return 0 if not failures else 1


sys.exit(main())
