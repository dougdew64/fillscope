# The attribution report (tools/attribution/attribution.py): which commits count as co-authored, how
# lines are credited, which files are counted, and the check of a committed report. Each test
# builds a small repository of its own.
#
#   python3 -I tests/attribution/attribution_test.py
import importlib.util
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

# Loading the script by path would otherwise leave __pycache__ files in the tree.
sys.dont_write_bytecode = True

SCRIPT = pathlib.Path(__file__).resolve().parents[2] / "tools" / "attribution" / "attribution.py"
spec = importlib.util.spec_from_file_location("attribution", SCRIPT)
attribution = importlib.util.module_from_spec(spec)
spec.loader.exec_module(attribution)

TRAILER = "\n\nCo-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
# Long, distinct lines: git blame -C follows a moved block only if it has enough characters.
BLOCK = "".join(f"int function_number_{i}(int argument) {{ return argument * {i} + {i * 7}; }}\n" for i in range(12))


class Repo:
    """A throwaway repository; the environment pins identities and dates and ignores the user's
    git configuration, so every run is the same."""

    def __init__(self, path):
        self.root = pathlib.Path(path)
        self.env = {**os.environ, "GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_NOSYSTEM": "1",
                    "GIT_AUTHOR_NAME": "Owner", "GIT_AUTHOR_EMAIL": "owner@example.com",
                    "GIT_COMMITTER_NAME": "Owner", "GIT_COMMITTER_EMAIL": "owner@example.com",
                    "GIT_AUTHOR_DATE": "2026-01-01T00:00:00Z", "GIT_COMMITTER_DATE": "2026-01-01T00:00:00Z"}
        self.git("init", "-q", "-b", "main")

    def git(self, *args):
        return subprocess.run(["git", "-C", str(self.root), *args], env=self.env, check=True,
                              capture_output=True, text=True).stdout

    def write(self, path, text):
        file = self.root / path
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_text(text)

    def commit(self, message, **files):
        for path, text in files.items():
            self.write(path.replace("__", "/"), text)
        self.git("add", "-A")
        self.git("commit", "-q", "--allow-empty", "-m", message)
        return self.git("rev-parse", "HEAD").strip()

    def counts(self, path, rev="HEAD"):
        commit = self.git("rev-parse", rev).strip()
        return attribution.blame_counts(self.root, commit, path, attribution.coauthored_commits(self.root, commit))


class TempRepoTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.repo = Repo(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()


class CoauthoredTest(TempRepoTest):
    def test_a_claude_trailer_makes_a_commit_coauthored_and_its_absence_the_owners(self):
        owner = self.repo.commit("Owner's", **{"a.cpp": "x\n"})
        coauthored = self.repo.commit("Claude's" + TRAILER, **{"b.cpp": "y\n"})
        self.assertEqual(attribution.coauthored_commits(self.repo.root, "HEAD"), {coauthored})
        self.assertNotIn(owner, attribution.coauthored_commits(self.repo.root, "HEAD"))

    def test_the_trailers_key_is_matched_in_any_case(self):
        sha = self.repo.commit("Lower case\n\nCo-authored-by: Claude <noreply@anthropic.com>", **{"a.cpp": "x\n"})
        self.assertEqual(attribution.coauthored_commits(self.repo.root, "HEAD"), {sha})

    def test_a_trailer_naming_someone_else_or_claude_in_the_body_is_the_owners(self):
        self.repo.commit("Pair\n\nCo-Authored-By: Claudette Smith <c@example.com>", **{"a.cpp": "x\n"})
        self.repo.commit("Mentions Claude\n\nClaude suggested this, Co-Authored-By: Claude in prose.",
                         **{"b.cpp": "y\n"})
        self.assertEqual(attribution.coauthored_commits(self.repo.root, "HEAD"), set())

    def test_claude_among_several_coauthors_counts(self):
        sha = self.repo.commit("Three\n\nCo-Authored-By: Someone <s@example.com>\n"
                               "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>", **{"a.cpp": "x\n"})
        self.assertEqual(attribution.coauthored_commits(self.repo.root, "HEAD"), {sha})


class BlameTest(TempRepoTest):
    def test_each_line_is_credited_to_the_commit_that_last_changed_it(self):
        self.repo.commit("Owner's", **{"a.cpp": "one\ntwo\nthree\n"})
        self.repo.commit("Claude's" + TRAILER, **{"a.cpp": "one\nTWO\nthree\nfour\n"})
        self.assertEqual(self.repo.counts("a.cpp"), (2, 2))

    def test_a_whitespace_only_change_keeps_the_lines_author(self):
        self.repo.commit("Owner's", **{"a.cpp": "int f() {\nreturn 1;\n}\n"})
        self.repo.commit("Reindented" + TRAILER, **{"a.cpp": "int f() {\n    return 1;\n}\n"})
        self.assertEqual(self.repo.counts("a.cpp"), (3, 0))

    def test_lines_moved_to_another_file_keep_their_author(self):
        self.repo.commit("Owner's", **{"a.cpp": "header\n" + BLOCK})
        self.repo.commit("Moved" + TRAILER, **{"a.cpp": "header\n", "b.cpp": BLOCK})
        self.assertEqual(self.repo.counts("b.cpp"), (len(BLOCK.splitlines()), 0))


class ReportTest(TempRepoTest):
    def test_a_submodule_is_not_counted(self):
        # A submodule is a commit of another repository, recorded as one entry (a gitlink); its
        # lines are that repository's report's to count.
        other = self.repo.commit("Other", **{"a.cpp": "x\n"})
        self.repo.git("update-index", "--add", "--cacheinfo", f"160000,{other},backend")
        self.repo.git("commit", "-q", "-m", "Submodule" + TRAILER)
        self.assertEqual(attribution.text_files(self.repo.root, "HEAD"), ["a.cpp"])
        self.assertNotIn("backend", attribution.report(self.repo.root, "HEAD"))

    def test_binary_files_symlinks_the_report_and_files_nobody_wrote_are_not_counted(self):
        self.repo.write("image.png", "")
        (self.repo.root / "image.png").write_bytes(b"\x89PNG\0\0binary")
        os.symlink("a.cpp", self.repo.root / "link.cpp")
        self.repo.commit("Files" + TRAILER, **{"a.cpp": "x\n", "LICENSE": "MIT\n", "docs__ATTRIBUTION.md": "old\n",
                                               "docs__guide.md": "# Guide\n"})
        self.assertEqual(attribution.text_files(self.repo.root, "HEAD"),
                         ["LICENSE", "a.cpp", "docs/ATTRIBUTION.md", "docs/guide.md"])
        text = attribution.report(self.repo.root, "HEAD")
        self.assertIn("| `a.cpp` | 0 | 1 |", text)
        self.assertNotIn("`docs/ATTRIBUTION.md`", text)
        self.assertIn("| `LICENSE` | the standard text of the MIT license |", text)
        self.assertNotIn("| `LICENSE` | 0", text)

    def test_totals_separate_code_from_documents(self):
        self.repo.commit("Owner's", **{"a.cpp": "1\n2\n3\n"})
        self.repo.commit("Claude's" + TRAILER, **{"b.cpp": "1\n", "docs__guide.md": "1\n2\n"})
        text = attribution.report(self.repo.root, "HEAD")
        self.assertIn("| Code, tests and configuration | 3 | 1 | 75.0% |", text)
        self.assertIn("| Documents (`.md`) | 0 | 2 | 0.0% |", text)

    def test_the_report_names_the_commit_it_describes(self):
        sha = self.repo.commit("First", **{"a.cpp": "x\n"})
        self.repo.commit("Second", **{"a.cpp": "y\n"})
        text = attribution.report(self.repo.root, sha)
        self.assertIn(f"As of commit `{sha}` (2026-01-01, \"First\")", text)
        self.assertIn("| `a.cpp` | 1 | 0 |", text)


class CheckTest(TempRepoTest):
    def publish(self, rev="HEAD"):
        self.assertEqual(attribution.main(["--root", str(self.repo.root), "--at", rev]), 0)
        return self.repo.commit("Report" + TRAILER)

    def test_a_report_regenerated_at_its_commit_passes(self):
        self.repo.commit("Owner's", **{"a.cpp": "x\n"})
        self.publish()
        self.repo.commit("Later", **{"a.cpp": "y\n"})  # the report may fall behind; it is still true
        self.assertIsNone(attribution.check(self.repo.root))

    def test_an_edited_number_fails(self):
        self.repo.commit("Owner's", **{"a.cpp": "x\n"})
        self.publish()
        path = self.repo.root / attribution.REPORT
        path.write_text(path.read_text().replace("| `a.cpp` | 1 | 0 |", "| `a.cpp` | 0 | 1 |"))
        self.assertIn("isn't what commit", attribution.check(self.repo.root))

    def test_a_commit_outside_heads_history_fails(self):
        self.repo.commit("Base", **{"a.cpp": "x\n"})
        self.repo.git("checkout", "-q", "-b", "side")
        side = self.repo.commit("Side", **{"a.cpp": "side\n"})
        self.repo.git("checkout", "-q", "main")
        self.assertEqual(attribution.main(["--root", str(self.repo.root), "--at", side]), 0)
        self.assertIn("isn't in HEAD's history", attribution.check(self.repo.root))

    def test_a_missing_report_or_one_naming_no_commit_fails(self):
        self.repo.commit("Owner's", **{"a.cpp": "x\n"})
        self.assertIn("doesn't exist", attribution.check(self.repo.root))
        self.repo.write(attribution.REPORT, "# Attribution\n")
        self.assertIn("names no commit", attribution.check(self.repo.root))

    def test_check_exits_nonzero_when_the_report_is_wrong(self):
        self.repo.commit("Owner's", **{"a.cpp": "x\n"})
        self.publish()
        self.assertEqual(attribution.main(["--root", str(self.repo.root), "--check"]), 0)
        (self.repo.root / attribution.REPORT).write_text("# Attribution\n")
        self.assertEqual(attribution.main(["--root", str(self.repo.root), "--check"]), 1)


if __name__ == "__main__":
    unittest.main()
