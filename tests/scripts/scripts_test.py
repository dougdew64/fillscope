# The test machinery's own logic (iterate.py): reading the manifest, what is never recorded as
# passed, the registration and sanitizer checks, fingerprints and the pass record. These decide
# whether a test runs and whether a pass counts, and a mistake in them would pass silently, so they
# are tested like the code they test.
#
#   python3 -I tests/scripts/scripts_test.py
import importlib.util
import io
import os
import pathlib
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

HERE = pathlib.Path(__file__).resolve().parent


def load_iterate(state=None):
    """A fresh copy of iterate.py as a module (python3 -I puts no script directory on the import
    path, so it is loaded by its path), with its pass record at `state` if given."""
    if state is not None:
        os.environ["FILLSCOPE_ITERATE_STATE"] = str(state)
    spec = importlib.util.spec_from_file_location("iterate", HERE / "iterate.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


iterate = load_iterate()


def manifest_line(name, tier, program, *runtime, always=False):
    return "\t".join([name, tier, "always" if always else "-", program, *runtime])


def test_entry(name, program, command):
    return {"name": name, "command": command,
            "properties": [{"name": "LABELS", "value": [f"exe:{program}", "unit"]}]}


class ManifestTest(unittest.TestCase):
    def test_a_line_names_the_program_its_tier_flags_and_run_time_files_even_with_spaces(self):
        [program] = iterate.programs(manifest_line("viewer_ui_tests", "ui", "/a b/viewer_ui_tests", "/a b/frame source",
                                                   always=True) + "\n")
        self.assertEqual(program.name, "viewer_ui_tests")
        self.assertEqual(program.tier, "ui")
        self.assertTrue(program.always)
        self.assertEqual(program.binary, "/a b/viewer_ui_tests")
        self.assertEqual(program.files, ["/a b/viewer_ui_tests", "/a b/frame source"])

    def test_a_command_has_no_binary(self):
        [program] = iterate.programs(manifest_line("vscode_debug_state_tests", "check", "-", "/x/a.js"))
        self.assertIsNone(program.binary)
        self.assertEqual(program.files, ["/x/a.js"])
        self.assertTrue(program.fast)
        [slow] = iterate.programs(manifest_line("lockstep_debugger_tests", "debugger", "/b"))
        self.assertFalse(slow.fast)


class NotRecordedTest(unittest.TestCase):
    everything = iterate.programs("\n".join([
        manifest_line("a_tests", "unit", "/a"), manifest_line("b_tests", "debugger", "/b"),
        manifest_line("c_tests", "unit", "/c"), manifest_line("d_tests", "unit", "/d")]))
    by_program = {"a_tests": ["A.One", "A.Two"], "b_tests": ["B.One", "B.Two"], "c_tests": ["C.One"]}

    def test_programs_that_tested_nothing_are_not_recorded(self):
        log = ("1/4 Test  #3: B.One ........***Skipped   0.01 sec\n"
               "2/4 Test  #4: B.Two ........***Skipped   0.01 sec\n"
               "3/4 Test  #1: A.One ........   Passed    0.01 sec\n"
               "4/4 Test  #2: A.Two ........***Skipped   0.01 sec\n")
        # b skipped every test; d has none at all; a skipped only some, and c skipped none.
        self.assertEqual(iterate.not_to_record(log, self.everything, self.by_program), {"b_tests", "d_tests"})


class RegistrationTest(unittest.TestCase):
    everything = iterate.programs("\n".join([manifest_line("a_tests", "unit", "/build/a_tests"),
                                             manifest_line("node_tests", "unit", "-", "/x.js")]))

    def problems(self, listing):
        return iterate.check_registration(self.everything, listing)

    def test_tests_registered_properly_pass(self):
        self.assertEqual(self.problems([test_entry("A.One", "a_tests", ["/build/a_tests", "--gtest_filter=A.One"]),
                                        test_entry("node_tests", "node_tests", ["flock", "node", "--test"])]), [])

    def test_a_test_without_a_program_is_found(self):
        problems = self.problems([test_entry("A.One", "a_tests", ["/build/a_tests"]),
                                  test_entry("node_tests", "node_tests", ["node"]),
                                  {"name": "stray", "command": ["true"], "properties": []}])
        self.assertEqual(len(problems), 1)
        self.assertIn("stray: not registered", problems[0])

    def test_a_test_that_copies_a_programs_labels_but_runs_something_else_is_found(self):
        problems = self.problems([test_entry("A.One", "a_tests", ["/build/a_tests"]),
                                  test_entry("copied", "a_tests", ["true"]),
                                  test_entry("node_tests", "node_tests", ["node"])])
        self.assertEqual(problems, ["copied: labelled a_tests, but doesn't run it (['true'])"])

    def test_a_program_with_no_tests_is_found(self):
        problems = self.problems([test_entry("node_tests", "node_tests", ["node"])])
        self.assertEqual(problems, ["a_tests: has no tests, so it would pass having tested nothing"])


class SanitizerTest(unittest.TestCase):
    both = "libasan.so.8 => /lib64/libasan.so.8\nlibubsan.so.1 => /lib64/libubsan.so.1\n"

    def test_the_sanitizer_build_needs_both(self):
        outputs = {"/both": self.both, "/asan_only": "libasan.so.8 => /lib64/libasan.so.8\n"}
        problems = iterate.sanitizer_problems(["/both", "/asan_only"], True, outputs.__getitem__)
        self.assertEqual(problems, ["/asan_only is in the sanitizer build but not linked with libubsan"])

    def test_the_normal_build_needs_neither(self):
        outputs = {"/plain": "libc.so.6 => /lib64/libc.so.6\n", "/both": self.both}
        problems = iterate.sanitizer_problems(["/plain", "/both"], False, outputs.__getitem__)
        self.assertEqual(len(problems), 2)


class FingerprintTest(unittest.TestCase):
    def test_a_missing_file_is_a_change_not_an_error(self):
        with tempfile.TemporaryDirectory() as scratch:
            path = pathlib.Path(scratch) / "lldb_driver.py"
            path.write_text("x")
            present = iterate.file_digest(str(path))
            path.unlink()
            self.assertEqual(iterate.file_digest(str(path)), b"missing")
            self.assertNotEqual(present, b"missing")

    def test_a_large_file_is_fingerprinted_by_its_size_and_time(self):
        with tempfile.TemporaryDirectory() as scratch:
            path = pathlib.Path(scratch) / "node"
            path.write_bytes(b"\0" * iterate.LARGE_FILE)
            self.assertEqual(iterate.file_digest(str(path)), iterate.stat_of(str(path)).encode())

    def test_werror_and_the_scripts_own_variables_dont_count(self):
        saved = dict(os.environ)
        try:
            os.environ.update({"FILLSCOPE_WERROR": "ON", "FILLSCOPE_ITERATE_STATE": "/x", "FILLSCOPE_REQUIRE_DEBUGGERS": "1"})
            self.assertEqual(iterate.environment(), "FILLSCOPE_REQUIRE_DEBUGGERS=1")
        finally:
            os.environ.clear()
            os.environ.update(saved)


class PassRecordTest(unittest.TestCase):
    def test_a_record_is_kept_and_an_unreadable_one_is_ignored_with_a_warning(self):
        with tempfile.TemporaryDirectory() as scratch:
            state = pathlib.Path(scratch) / "last-pass.json"
            module = load_iterate(state)
            module.record({"a_tests": "f1"})
            module.record({"b_tests": "f2"})
            self.assertEqual(module.load_state(), {"a_tests": "f1", "b_tests": "f2"})
            self.assertEqual([p.name for p in pathlib.Path(scratch).iterdir()], ["last-pass.json"],
                             "no temporary file left")
            state.write_text("{ torn")
            said = io.StringIO()
            with redirect_stdout(said):
                self.assertEqual(module.load_state(), {})
            self.assertIn("ignoring the unreadable pass record", said.getvalue())


if __name__ == "__main__":
    result = unittest.main(exit=False, verbosity=2).result
    sys.exit(0 if result.wasSuccessful() and result.testsRun > 0 else 1)
