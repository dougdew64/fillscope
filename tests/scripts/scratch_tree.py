# A scratch copy of the working tree, with a build of its own, for the checks that change source
# files (mutants.py, selection_check.py). They work only in the copy, so they never touch the files
# being edited, nor the build the editor and iterate.py use, and an interrupted check leaves
# nothing behind that matters: the copy is in a temporary directory and is deleted afterwards.
#
# The copy holds the working tree as it is, uncommitted changes included: every file git tracks or
# would track (untracked files that aren't ignored), the back end's (a submodule's) files likewise,
# plus the extension's installed development tools. It is made a git repository of its own, since iterate.py asks git which build files exist,
# with the real repository's history (below).
# Its build reuses the dependencies the real debug build has already fetched, when it has the same
# release (checked by its SHA-256), rather than fetching them again.
import os
import pathlib
import re
import shutil
import signal
import subprocess
import tempfile

# FetchContent's names for the dependencies, and the directories the real build fetched them into.
DEPENDENCIES = {
    "IMGUI": "imgui-src",
    "JSON": "json-src",
    "GOOGLETEST": "googletest-src",
    "IMGUI_TEST_ENGINE": "imgui_test_engine-src",
}
NODE_MODULES = pathlib.Path("tools") / "vscode-debug-state" / "node_modules"


def tracked_files(root):
    """The working tree's files: tracked, or untracked and not ignored, and present now."""
    listed = subprocess.run(["git", "-C", str(root), "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
                            stdout=subprocess.PIPE, check=True).stdout.decode()
    return [name for name in listed.split("\0") if name and (root / name).exists()]


def copy_file(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    if source.is_symlink():
        target.symlink_to(os.readlink(source))
    else:
        shutil.copy2(source, target)


def make(root):
    """Copies the working tree at `root` into a new temporary directory, and returns its path.
    Modification times are kept, so the copy's build sees the files as they are. If copying fails
    or is interrupted, the partial copy is removed."""
    copy = pathlib.Path(tempfile.mkdtemp(prefix="fillscope-check-"))
    try:
        fill(root, copy)
    except BaseException:  # KeyboardInterrupt and SystemExit (a signal) too
        remove(copy)
        raise
    return copy


def fill(root, copy):
    for name in tracked_files(root):
        source = root / name
        if source.is_dir() and not source.is_symlink():
            # A submodule (backend/): git lists it as one entry. Its own files are copied, and in
            # the copy they are plain files of the copy's repository.
            for inner in tracked_files(source):
                copy_file(source / inner, copy / name / inner)
        else:
            copy_file(source, copy / name)
    if (root / NODE_MODULES).is_dir():
        shutil.copytree(root / NODE_MODULES, copy / NODE_MODULES, symlinks=True)
    quiet = {"stdout": subprocess.DEVNULL, "stderr": subprocess.DEVNULL, "check": True}
    subprocess.run(["git", "-C", str(copy), "init", "-q"], **quiet)
    # The real repository's history too, borrowed rather than copied: its objects through an
    # alternates file, and its HEAD's commit as the copy's. The attribution check blames that
    # history.
    head = subprocess.run(["git", "-C", str(root), "rev-parse", "--verify", "-q", "HEAD"],
                          stdout=subprocess.PIPE, text=True).stdout.strip()
    if head:
        objects = subprocess.run(["git", "-C", str(root), "rev-parse", "--path-format=absolute", "--git-common-dir"],
                                 stdout=subprocess.PIPE, text=True, check=True).stdout.strip()
        (copy / ".git" / "objects" / "info" / "alternates").write_text(f"{objects}/objects\n")
        subprocess.run(["git", "-C", str(copy), "update-ref", "HEAD", head], **quiet)
    subprocess.run(["git", "-C", str(copy), "add", "-A"], **quiet)


def declared_hashes(copy):
    """The SHA-256 of every release the copy's build files declare (FetchContent_Declare)."""
    return {match for path in (copy / "CMakeLists.txt", copy / "tests" / "CMakeLists.txt") if path.exists()
            for match in re.findall(r"URL_HASH SHA256=([0-9a-f]{64})", path.read_text())}


def fetched_hash(root, directory):
    """The SHA-256 the real build fetched `directory` (e.g. imgui-src) with, from its sub-build."""
    subbuild = root / "build" / "debug" / "_deps" / directory.replace("-src", "-subbuild") / "CMakeLists.txt"
    found = re.search(r"SHA256=([0-9a-f]{64})", subbuild.read_text()) if subbuild.exists() else None
    return found.group(1) if found else None


def configure(copy, root):
    """Configures the copy's debug build, without -Werror (a deliberately broken file may well draw
    a warning), using the dependencies the real build fetched, if it has. Returns the build
    directory, or None if configuring failed."""
    build = copy / "build" / "debug"
    command = ["cmake", "-S", str(copy), "-B", str(build), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Debug",
               "-DFILLSCOPE_WERROR=OFF"]
    wanted = declared_hashes(copy)
    for name, directory in DEPENDENCIES.items():
        fetched = root / "build" / "debug" / "_deps" / directory
        # Only the release the copy declares: after a dependency is bumped, and before the real
        # build is rebuilt, its directory holds the old one, which the copy then fetches afresh.
        if fetched.is_dir() and fetched_hash(root, directory) in wanted:
            command.append(f"-DFETCHCONTENT_SOURCE_DIR_{name}={fetched}")
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return build if result.returncode == 0 else None


def run(args, **options):
    """subprocess.run, with the command and everything it starts in a process group of their own,
    killed if the check is stopped: a test's exchange, started by ctest, would otherwise outlive it
    and keep its fixed ports."""
    process = subprocess.Popen(args, start_new_session=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, **options)
    try:
        output, _ = process.communicate()
    except BaseException:  # KeyboardInterrupt and SystemExit (a signal) too
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait()
        raise
    return subprocess.CompletedProcess(args, process.returncode, output)


def remove(copy):
    shutil.rmtree(copy, ignore_errors=True)
