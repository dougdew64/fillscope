# A scratch copy of the working tree, with a build of its own, for the checks that change source
# files (mutants.py, selection_check.py). They work only in the copy, so they never touch the files
# being edited, nor the build the editor and iterate.py use, and an interrupted check leaves
# nothing behind that matters: the copy is in a temporary directory and is deleted afterwards.
#
# The copy holds the working tree as it is, uncommitted changes included: every file git tracks or
# would track (untracked files that aren't ignored), plus the extension's installed development
# tools. It is made a git repository of its own, since iterate.py asks git which build files exist.
# Its build reuses the dependencies the real debug build has already fetched, when it has, rather
# than fetching them again.
import os
import pathlib
import shutil
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


def make(root):
    """Copies the working tree at `root` into a new temporary directory, and returns its path.
    Modification times are kept, so the copy's build sees the files as they are."""
    copy = pathlib.Path(tempfile.mkdtemp(prefix="fillscope-check-"))
    for name in tracked_files(root):
        source = root / name
        target = copy / name
        target.parent.mkdir(parents=True, exist_ok=True)
        if source.is_symlink():
            target.symlink_to(os.readlink(source))
        else:
            shutil.copy2(source, target)
    if (root / NODE_MODULES).is_dir():
        shutil.copytree(root / NODE_MODULES, copy / NODE_MODULES, symlinks=True)
    quiet = {"stdout": subprocess.DEVNULL, "stderr": subprocess.DEVNULL, "check": True}
    subprocess.run(["git", "-C", str(copy), "init", "-q"], **quiet)
    subprocess.run(["git", "-C", str(copy), "add", "-A"], **quiet)
    return copy


def configure(copy, root):
    """Configures the copy's debug build, without -Werror (a deliberately broken file may well draw
    a warning), using the dependencies the real build fetched, if it has. Returns the build
    directory, or None if configuring failed."""
    build = copy / "build" / "debug"
    command = ["cmake", "-S", str(copy), "-B", str(build), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Debug",
               "-DFILLSCOPE_WERROR=OFF"]
    for name, directory in DEPENDENCIES.items():
        fetched = root / "build" / "debug" / "_deps" / directory
        if fetched.is_dir():
            command.append(f"-DFETCHCONTENT_SOURCE_DIR_{name}={fetched}")
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return build if result.returncode == 0 else None


def remove(copy):
    shutil.rmtree(copy, ignore_errors=True)
