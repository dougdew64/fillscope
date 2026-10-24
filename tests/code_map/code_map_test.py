# The code map's check (tools/code_map/code_map.py): reading the map, which files count as code,
# and what it reports. The repository's own map is checked by code_map_check (CMakeLists.txt).
#
#   python3 -I tests/code_map/code_map_test.py
import importlib.util
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

# Loading the script by path would otherwise leave __pycache__ files in the tree.
sys.dont_write_bytecode = True

SCRIPT = pathlib.Path(__file__).resolve().parents[2] / "tools" / "code_map" / "code_map.py"
spec = importlib.util.spec_from_file_location("code_map", SCRIPT)
code_map = importlib.util.module_from_spec(spec)
spec.loader.exec_module(code_map)

MAP = """# A map
Some text with `not/a/pattern.cpp` in it.

## Fillscope
| Part | Files | Explained in | Asked about |
|---|---|---|---|
| Books | `src/feed/book.*` | §3 `book.h` | |
| Viewer | `src/viewer/`, `CMakeLists.txt` | §4 | 2026-10-24: a question |

## The back end (`backend/Chapter12/`)
| Part | Files | Explained in | Asked about |
|---|---|---|---|
| Matcher | `exchange/matcher/` | not yet | |
"""


def git(root, *args):
    env = {**os.environ, "GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_NOSYSTEM": "1"}
    subprocess.run(["git", "-C", str(root), *args], env=env, check=True, capture_output=True)


def write(root, path, text=""):
    file = root / path
    file.parent.mkdir(parents=True, exist_ok=True)
    file.write_text(text)


class ParseMap(unittest.TestCase):
    def test_reads_each_rows_patterns_from_its_files_cell(self):
        self.assertEqual(code_map.parse_map(MAP), [
            ("Books", ["src/feed/book.*"]),
            ("Viewer", ["src/viewer/", "CMakeLists.txt"]),
            ("Matcher", ["backend/Chapter12/exchange/matcher/"]),
        ])

    def test_a_heading_without_a_directory_resets_the_base(self):
        text = "## Back end (`backend/Chapter12/`)\n| A | `x.cpp` |\n## Other\n| B | `y.cpp` |\n"
        self.assertEqual(code_map.parse_map(text), [("A", ["backend/Chapter12/x.cpp"]), ("B", ["y.cpp"])])

    def test_a_row_without_files_is_kept_so_it_is_reported(self):
        self.assertEqual(code_map.parse_map("| Part | Files |\n|---|---|\n| Empty | none yet |\n"), [("Empty", [])])


class Matches(unittest.TestCase):
    def test_a_directory_matches_everything_under_it_and_nothing_beside_it(self):
        self.assertTrue(code_map.matches("src/feed/", "src/feed/book.h"))
        self.assertTrue(code_map.matches("src/feed/", "src/feed/deeper/file.h"))
        self.assertFalse(code_map.matches("src/feed/", "src/feedback.cpp"))

    def test_a_file_pattern_matches_as_a_shell_does(self):
        self.assertTrue(code_map.matches("src/feed/book.*", "src/feed/book.cpp"))
        self.assertFalse(code_map.matches("src/feed/book.*", "src/feed/book_builder.cpp"))
        self.assertFalse(code_map.matches("CMakeLists.txt", "tests/CMakeLists.txt"))


class IsCode(unittest.TestCase):
    def test_documents_and_the_license_are_not_code(self):
        for path in ("README.md", "docs/ARCHITECTURE.md", "backend/Chapter12/README.md", "LICENSE"):
            self.assertFalse(code_map.is_code(path), path)

    def test_of_the_back_end_only_the_chapter_in_use_is_code(self):
        self.assertTrue(code_map.is_code("backend/Chapter12/exchange/exchange_main.cpp"))
        self.assertFalse(code_map.is_code("backend/Chapter10/exchange/exchange_main.cpp"))
        self.assertFalse(code_map.is_code("backend/tools/attribution/attribution.py"))
        self.assertTrue(code_map.is_code("src/viewer/main.cpp"))


class Problems(unittest.TestCase):
    FILES = ["src/feed/book.cpp", "src/feed/book.h", "src/viewer/main.cpp", "CMakeLists.txt",
             "backend/Chapter12/exchange/matcher/me_order.cpp", "docs/PLAN.md", "LICENSE",
             "backend/Chapter10/common/types.h"]

    def test_a_map_naming_every_code_file_once_has_no_problems(self):
        self.assertEqual(code_map.problems(code_map.parse_map(MAP), self.FILES), [])

    def test_a_code_file_in_no_part(self):
        found = code_map.problems(code_map.parse_map(MAP), self.FILES + ["src/common/fd.h"])
        self.assertEqual(found, ["src/common/fd.h: in no part"])

    def test_a_code_file_in_two_parts(self):
        parts = code_map.parse_map(MAP) + [("Again", ["src/viewer/main.cpp"])]
        self.assertEqual(code_map.problems(parts, self.FILES),
                         ["src/viewer/main.cpp: in more than one part (Viewer, Again)"])

    def test_a_pattern_that_names_no_file(self):
        parts = code_map.parse_map(MAP) + [("Gone", ["src/channel/"])]
        self.assertEqual(code_map.problems(parts, self.FILES), ["Gone: `src/channel/` names no code file"])

    def test_a_pattern_that_names_only_documents_names_no_code_file(self):
        parts = code_map.parse_map(MAP) + [("Plans", ["docs/"])]
        self.assertEqual(code_map.problems(parts, self.FILES), ["Plans: `docs/` names no code file"])

    def test_a_part_with_no_files(self):
        parts = code_map.parse_map(MAP) + [("Empty", [])]
        self.assertEqual(code_map.problems(parts, self.FILES), ["Empty: lists no files"])


class RepositoryFiles(unittest.TestCase):
    def test_lists_tracked_and_untracked_files_and_a_nested_repositorys_not_ignored_or_deleted_ones(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            git(root, "init", "-q")
            write(root, ".gitignore", "build/\n")
            write(root, "src/a.cpp")
            write(root, "src/gone.cpp")
            git(root, "add", "-A")
            os.remove(root / "src" / "gone.cpp")  # tracked, but deleted in the working tree
            write(root, "src/new.cpp")  # untracked
            write(root, "build/out.o")  # ignored
            # A nested repository, as a submodule is: git lists it as one entry.
            write(root, "backend/Chapter12/b.cpp")
            git(root / "backend", "init", "-q")
            git(root / "backend", "add", "-A")
            git(root / "backend", "-c", "user.name=Test", "-c", "user.email=test@example.com", "commit", "-q", "-m", "b")
            git(root, "add", "backend")
            self.assertEqual(code_map.repository_files(root),
                             [".gitignore", "backend/Chapter12/b.cpp", "src/a.cpp", "src/new.cpp"])


class Main(unittest.TestCase):
    def run_check(self, map_text):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            git(root, "init", "-q")
            write(root, "docs/CODE-MAP.md", map_text)
            write(root, "src/viewer/main.cpp")
            return code_map.main(["--check", str(root)])

    def test_passes_a_complete_map(self):
        self.assertEqual(self.run_check("| Part | Files |\n|---|---|\n| Viewer | `src/viewer/` |\n"), 0)

    def test_fails_an_incomplete_map(self):
        self.assertEqual(self.run_check("| Part | Files |\n|---|---|\n| Other | `src/other/` |\n"), 1)


if __name__ == "__main__":
    unittest.main()
