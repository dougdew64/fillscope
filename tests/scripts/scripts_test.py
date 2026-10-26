# The test machinery's own logic: iterate.py's (reading the manifest, what is never recorded as
# passed, the registration and sanitizer checks, fingerprints and the pass record), and the mutation
# check's choice of which mutants to run (mutants.py's keys and record, scratch_tree.py's sync).
# These decide whether a test or a mutant runs and whether a pass counts, and a mistake in them
# would pass silently, so they are tested like the code they test.
#
#   python3 -I tests/scripts/scripts_test.py
import hashlib
import importlib.util
import io
import os
import pathlib
import subprocess
import sys
import tempfile
import time
import unittest
from contextlib import redirect_stdout

# Loading the scripts beside this one by path would otherwise leave __pycache__ files in the tree.
sys.dont_write_bytecode = True

HERE = pathlib.Path(__file__).resolve().parent


def load_iterate(state=None):
    """A fresh copy of iterate.py as a module (python3 -I puts no script directory on the import
    path, so it is loaded by its path), with its pass record at `state` if given."""
    previous = os.environ.get("FILLSCOPE_ITERATE_STATE")
    if state is not None:
        os.environ["FILLSCOPE_ITERATE_STATE"] = str(state)
    try:
        spec = importlib.util.spec_from_file_location("iterate", HERE / "iterate.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
    finally:  # read when the module loads; restored, so it doesn't leak into later tests
        if previous is None:
            os.environ.pop("FILLSCOPE_ITERATE_STATE", None)
        else:
            os.environ["FILLSCOPE_ITERATE_STATE"] = previous
    return module


iterate = load_iterate()


def manifest_line(name, tier, program, *runtime, always=False):
    return "\t".join([name, tier, "always" if always else "-", program, *runtime])


def test_entry(name, program, command):
    return {"name": name, "command": command,
            "properties": [{"name": "LABELS", "value": [f"exe:{program}", "unit"]}]}


class ManifestTest(unittest.TestCase):
    def test_a_line_names_the_program_its_tier_flags_and_run_time_files_even_with_spaces(self):
        [program] = iterate.programs(manifest_line("viewer_ui_tests", "ui", "/a b/viewer_ui_tests", "/a b/exchange replay",
                                                   always=True) + "\n")
        self.assertEqual(program.name, "viewer_ui_tests")
        self.assertEqual(program.tier, "ui")
        self.assertTrue(program.always)
        self.assertEqual(program.binary, "/a b/viewer_ui_tests")
        self.assertEqual(program.files, ["/a b/viewer_ui_tests", "/a b/exchange replay"])

    def test_a_command_has_no_binary(self):
        [program] = iterate.programs(manifest_line("vscode_debug_state_tests", "check", "-", "/x/a.js"))
        self.assertIsNone(program.binary)
        self.assertEqual(program.files, ["/x/a.js"])
        self.assertTrue(program.fast)
        [slow] = iterate.programs(manifest_line("replay_debugger_tests", "debugger", "/b"))
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
            path = pathlib.Path(scratch) / "replay_lldb_driver.py"
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
            # Only the variables set here: any set outside, by a caller, mustn't change the result.
            for key in [key for key in os.environ if key.startswith("FILLSCOPE_")]:
                del os.environ[key]
            os.environ.update({"FILLSCOPE_WERROR": "ON", "FILLSCOPE_ITERATE_STATE": "/x", "FILLSCOPE_MUTANTS_DIR": "/y",
                               "FILLSCOPE_JOBS": "4", "FILLSCOPE_REQUIRE_DEBUGGERS": "1"})
            self.assertEqual(iterate.environment(), "FILLSCOPE_REQUIRE_DEBUGGERS=1")
        finally:
            os.environ.clear()
            os.environ.update(saved)


def listed_test(name, program, timeout=2.0, backtrace=1):
    """One test as CTest's --show-only=json-v1 lists it."""
    return {"backtrace": backtrace, "command": [f"/build/{program}", f"--gtest_filter={name}"], "name": name,
            "properties": [{"name": "LABELS", "value": [f"exe:{program}", "unit"]},
                           {"name": "TIMEOUT", "value": timeout}]}


class HowTestsRunTest(unittest.TestCase):
    """A build file counts only through what it decides: how CTest runs each program's tests."""

    def fingerprints(self, listing):
        programs = [iterate.Program(manifest_line(name, "unit", "-")) for name in ("viewer_tests", "feed_tests")]
        return iterate.fingerprints(programs, listing)

    def test_a_tests_registration_changes_only_its_programs_fingerprint(self):
        before = self.fingerprints([listed_test("A.One", "viewer_tests"), listed_test("B.One", "feed_tests")])
        after = self.fingerprints([listed_test("A.One", "viewer_tests", timeout=10.0), listed_test("B.One", "feed_tests")])
        self.assertNotEqual(after["viewer_tests"], before["viewer_tests"])
        self.assertEqual(after["feed_tests"], before["feed_tests"])

    def test_a_test_added_changes_its_programs_fingerprint(self):
        before = self.fingerprints([listed_test("A.One", "viewer_tests")])
        after = self.fingerprints([listed_test("A.One", "viewer_tests"), listed_test("A.Two", "viewer_tests")])
        self.assertNotEqual(after["viewer_tests"], before["viewer_tests"])

    def test_where_a_build_file_registered_a_test_doesnt_count(self):
        before = self.fingerprints([listed_test("A.One", "viewer_tests", backtrace=1)])
        after = self.fingerprints([listed_test("A.One", "viewer_tests", backtrace=7)])
        self.assertEqual(after, before)

    def test_the_order_ctest_lists_tests_in_doesnt_count(self):
        tests = [listed_test("A.One", "viewer_tests"), listed_test("A.Two", "viewer_tests")]
        self.assertEqual(self.fingerprints(tests), self.fingerprints(tests[::-1]))


class BinaryFingerprintTest(unittest.TestCase):
    def test_a_large_binary_is_hashed_by_its_contents(self):
        with tempfile.TemporaryDirectory() as scratch:
            path = pathlib.Path(scratch) / "viewer_ui_tests"
            path.write_bytes(b"\1" * iterate.LARGE_FILE)
            self.assertEqual(iterate.file_digest(str(path), whole=True), hashlib.sha256(path.read_bytes()).digest())

    def test_a_program_whose_bytes_change_is_changed_though_its_size_and_time_are_kept(self):
        with tempfile.TemporaryDirectory() as scratch:
            path = pathlib.Path(scratch) / "lowlatency_tests"
            path.write_bytes(b"\1" * iterate.LARGE_FILE)
            program = iterate.Program(manifest_line("lowlatency_tests", "system", str(path)))
            before = iterate.fingerprint(program, "common")
            times = path.stat()
            path.write_bytes(b"\2" * iterate.LARGE_FILE)
            os.utime(path, ns=(times.st_atime_ns, times.st_mtime_ns))
            self.assertNotEqual(iterate.fingerprint(program, "common"), before)


class PassRecordTest(unittest.TestCase):
    def test_the_record_drops_programs_the_build_no_longer_lists(self):
        with tempfile.TemporaryDirectory() as scratch:
            module = load_iterate(pathlib.Path(scratch) / "last-pass.json")
            module.record({"retired_tests": "f1", "kept_tests": "f2"})
            module.record({"new_tests": "f3"}, known={"kept_tests", "new_tests"})
            self.assertEqual(module.load_state(), {"kept_tests": "f2", "new_tests": "f3"})

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


def load_script(name):
    spec = importlib.util.spec_from_file_location(name, HERE / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


mutants = load_script("mutants")
scratch_tree = load_script("scratch_tree")


class MutantRecordTest(unittest.TestCase):
    """mutants.py skips a mutant whose key is unchanged since it was caught; a key that missed
    something it depends on would skip a mutant silently."""

    MUTANT = ("a full queue is overwritten", "backend/Chapter12/common/lf_queue.h",
              [("ASSERT(num_elements_ != store_.size(),", "ASSERT(true,")], "lowlatency:LFQueue.*")

    def key(self, machinery="m", mutant=MUTANT, contents=b"c", fingerprints=None):
        return mutants.mutant_key(machinery, mutant, contents,
                                  {"lowlatency_tests": "f"} if fingerprints is None else fingerprints)

    def test_a_key_is_the_same_for_the_same_things(self):
        self.assertEqual(self.key(), self.key())

    def test_a_key_changes_with_each_thing_a_result_depends_on(self):
        name, file, edits, tests = self.MUTANT
        changes = {
            "the check's code": self.key(machinery="m2"),
            "the mutant's edits": self.key(mutant=(name, file, [(edits[0][0], "ASSERT(false,")], tests)),
            "the tests it names": self.key(mutant=(name, file, edits, "lowlatency:LFQueue.AQueue*")),
            "the file it mutates": self.key(contents=b"c2"),
            "a program's fingerprint": self.key(fingerprints={"lowlatency_tests": "f2"}),
            "another program": self.key(fingerprints={"lowlatency_tests": "f", "feed_tests": "g"}),
        }
        for what, key in changes.items():
            self.assertNotEqual(key, self.key(), what)

    # A file's name, a mutant and the list's end, as mutants.py lays them out.
    SOURCE = "a = 1\n\n# The mutants: and their files.\nF = \"src/f.cpp\"\nMUTANTS = [\n    (\"one\", F),\n]\n\nb = 2\n"

    def test_the_check_s_code_leaves_out_the_mutants_and_their_files_but_nothing_else(self):
        source = self.SOURCE
        self.assertEqual(mutants.without_mutants(source), mutants.without_mutants(source.replace("one", "two")))
        self.assertEqual(mutants.without_mutants(source),
                         mutants.without_mutants(source.replace("F = ", "G = \"src/g.cpp\"\nF = ")))
        self.assertNotEqual(mutants.without_mutants(source), mutants.without_mutants(source.replace("b = 2", "b = 3")))
        self.assertNotEqual(mutants.without_mutants(source), mutants.without_mutants(source.replace("a = 1", "a = 3")))
        self.assertEqual(mutants.without_mutants(source), "a = 1\n\nb = 2\n")

    def test_the_check_s_own_code_uses_no_name_defined_among_the_mutants(self):
        self.assertIsNone(mutants.names_used_elsewhere(self.SOURCE))
        self.assertIsNone(mutants.names_used_elsewhere(pathlib.Path(mutants.__file__).read_text()))
        self.assertIn(": F", mutants.names_used_elsewhere(self.SOURCE.replace("b = 2", "b = F")))

    def test_the_tests_named_are_mapped_to_the_programs_that_run_them(self):
        asked = []

        def ctest_programs(regex):
            asked.append(regex)
            return {"feed_tests"} if regex.startswith("FeedSocket") else {"backend_system_tests"}
        self.assertEqual(mutants.programs_named("ui:clicking_an_order_points_at_it", ctest_programs), {"viewer_ui_tests"})
        self.assertEqual(mutants.programs_named("node:a session ends", ctest_programs), {"vscode_debug_state_tests"})
        self.assertEqual(mutants.programs_named("lowlatency:LFQueue.*", ctest_programs), {"lowlatency_tests"})
        self.assertEqual(mutants.programs_named("FeedSocket.X + backend_system_tests", ctest_programs),
                         {"feed_tests", "backend_system_tests"})
        self.assertEqual(asked, ["FeedSocket.X", "backend_system_tests"])

    def test_a_caught_mutant_is_recorded_and_one_not_caught_loses_its_record(self):
        record = mutants.updated({}, "a", "k1", caught=True)
        self.assertEqual(record, {"a": "k1"})
        self.assertEqual(mutants.updated(record, "a", "k2", caught=False), {})
        self.assertEqual(mutants.updated(record, "b", "k3", caught=False), {"a": "k1"})

    def test_every_mutant_has_a_name_of_its_own(self):
        self.assertIsNone(mutants.duplicate_names(mutants.MUTANTS))
        self.assertIn("twice", mutants.duplicate_names([("twice", "f", [], "t"), ("twice", "g", [], "t")]))


class SyncTest(unittest.TestCase):
    """scratch_tree.sync keeps the mutation check's copy up to date, changing only what differs, and
    giving what it rewrites the time now: the build rebuilds what is newer than what it built."""

    def test_only_what_differs_changes_and_a_rewritten_file_is_newer_than_before(self):
        with tempfile.TemporaryDirectory() as scratch:
            root, copy = pathlib.Path(scratch) / "root", pathlib.Path(scratch) / "copy"
            root.mkdir()
            subprocess.run(["git", "-C", str(root), "init", "-q"], check=True)
            for name in ("same.cpp", "edited.cpp", "removed.cpp"):
                (root / name).write_text(name)
            scratch_tree.sync(root, copy)
            self.assertEqual((copy / "edited.cpp").read_text(), "edited.cpp")
            old = time.time_ns() - 3600 * 10**9  # an hour ago: older than anything the copy built
            same_time = (copy / "same.cpp").stat().st_mtime_ns
            (root / "edited.cpp").write_text("edited, with new contents")
            os.utime(root / "edited.cpp", ns=(old, old))
            (root / "removed.cpp").unlink()
            (root / "added.cpp").write_text("added")
            before = time.time_ns()
            changed = scratch_tree.sync(root, copy)
            self.assertEqual(sorted(changed), ["added.cpp", "edited.cpp", "removed.cpp"])
            self.assertEqual((copy / "edited.cpp").read_text(), "edited, with new contents")
            self.assertGreaterEqual((copy / "edited.cpp").stat().st_mtime_ns, before,
                                    "a rewritten file keeps its source's old time, so the build wouldn't rebuild it")
            self.assertEqual((copy / "same.cpp").stat().st_mtime_ns, same_time, "an unchanged file was rewritten")
            self.assertFalse((copy / "removed.cpp").exists())
            self.assertEqual((copy / "added.cpp").read_text(), "added")


if __name__ == "__main__":
    result = unittest.main(exit=False, verbosity=2).result
    sys.exit(0 if result.wasSuccessful() and result.testsRun > 0 else 1)
