# The viewer program itself (src/viewer/main.cpp), as a user runs it: its window, its paint loop,
# its report lines, and every way it ends. The UI tests and the demos hold the viewer's ViewerApp in
# a window of their own, so this is the one test of main: run on the real exchange and a client
# trading at random, it must take in their market data, paint about 60 times a second, report once
# a second, and end on --exit-after, SIGTERM or SIGINT with a last context file saying it has
# stopped; --screenshot must write the window's last frame; a bad command line must be refused.
# And its log (src/viewer/viewer_log.h) must say, for each way it ends, how it ended: killed
# outright, it has everything up to then; crashed, the crash.
#
#   python3 -I tests/viewer/viewer_program_test.py VIEWER EXCHANGE_MAIN TRADING_MAIN
#
# It needs a display for the viewer's window: an Xvfb of its own, started here, so no window ever
# appears on the screen. Never the desktop's: its windows covered the user's. Without Xvfb, the
# tests that open a window are skipped (and fail, if FILLSCOPE_REQUIRE_DISPLAY is set, as in CI), and
# the program exits 77, which CTest reports as skipped.
import fcntl
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import unittest

VIEWER, EXCHANGE, TRADING = (os.path.abspath(path) for path in sys.argv[1:4])


def claim_slot():
    """The slot this process holds, 0 to 999, by RUNNING.md's rule: the first of the files
    fillscope-test-ports/slot-N in the temporary directory it can lock, held until it exits."""
    directory = os.path.join(tempfile.gettempdir(), "fillscope-test-ports")
    os.makedirs(directory, exist_ok=True)
    for slot in range(1000):
        fd = os.open(os.path.join(directory, f"slot-{slot}"), os.O_RDWR | os.O_CREAT | os.O_CLOEXEC, 0o666)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            return slot  # the descriptor stays open, and the lock held, until the process exits
        except BlockingIOError:
            os.close(fd)
    raise RuntimeError(f"every slot in {directory} is taken")


SLOT = claim_slot()
ORDER_PORT = 22000 + 10 * SLOT
INCREMENTAL = f"233.252.14.3:{22000 + 10 * SLOT + 1}"
SNAPSHOT = f"233.252.14.1:{22000 + 10 * SLOT + 2}"
ENDPOINTS = ["--order-port", str(ORDER_PORT), "--incremental", INCREMENTAL, "--snapshot", SNAPSHOT]
STREAMS = ["--incremental", INCREMENTAL, "--snapshot", SNAPSHOT]
# The viewer's line each second: "viewer: t=… paints=… updates=… books as of update …".
REPORT = re.compile(r"^viewer: t=(\d+) paints=(\d+) updates=(\d+) books as of update (\d+)$")
# The limits allow for the sanitizer build, software OpenGL (Xvfb's), and a loaded machine.
START_LIMIT = 30.0


class Display:
    """The display the viewer's window opens on: an Xvfb of this test's own, if Xvfb is installed.
    `env` is the viewer's environment, or None if there is no display."""

    def __init__(self):
        self.xvfb = None
        self.env = None
        self.runtime = None
        if shutil.which("Xvfb"):
            read, write = os.pipe()
            # -displayfd: Xvfb picks a free display number and writes it to the pipe once it is ready.
            # A small screen, so the window is small and software OpenGL draws it quickly: a viewer
            # that didn't pace itself would paint far more than 60 times a second, where on a large
            # one each frame took long enough that it didn't.
            self.xvfb = subprocess.Popen(["Xvfb", "-displayfd", str(write), "-screen", "0", "800x600x24", "-nolisten", "tcp"],
                                         pass_fds=[write], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            os.close(write)
            with os.fdopen(read) as pipe:
                number = pipe.readline().strip()
            if number:
                # The Xvfb as the X display, and no Wayland reachable at all. On a Wayland desktop, GLFW
                # tries Wayland first: with WAYLAND_DISPLAY unset, Wayland's library still finds the
                # desktop's socket (wayland-0) in XDG_RUNTIME_DIR, and the window opened on the
                # desktop; with WAYLAND_DISPLAY naming no socket, GLFW fails rather than turn to X11.
                # So the runtime directory is an empty one of the test's own: Wayland finds nothing,
                # and GLFW uses X11, the Xvfb (checked: the viewer's one display connection is to it).
                env = {k: v for k, v in os.environ.items() if k != "WAYLAND_DISPLAY"}
                env["DISPLAY"] = f":{number}"
                self.runtime = tempfile.TemporaryDirectory()
                os.chmod(self.runtime.name, 0o700)
                env["XDG_RUNTIME_DIR"] = self.runtime.name
                self.env = env

    def close(self):
        if self.xvfb is not None:
            self.xvfb.kill()
            self.xvfb.wait()
        if self.runtime is not None:
            self.runtime.cleanup()


DISPLAY = None  # made in main, before the tests run


def needs_display(test):
    if DISPLAY.env is None:
        if os.environ.get("FILLSCOPE_REQUIRE_DISPLAY"):
            test.fail("no display for the viewer's window (Xvfb isn't installed), and this run requires one")
        test.skipTest("no display for the viewer's window: Xvfb isn't installed (sudo dnf install xorg-x11-server-Xvfb)")


def order_port_open():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        return probe.connect_ex(("127.0.0.1", ORDER_PORT)) == 0


class Process:
    """A back-end program in its own process group, in `cwd` (it writes its logs there); killed at the
    end, since how it shuts down isn't the subject, and takes a minute."""

    def __init__(self, args, cwd):
        self.process = subprocess.Popen(args, cwd=cwd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                        start_new_session=True)

    def running(self):
        return self.process.poll() is None

    def kill(self):
        if self.running():
            os.killpg(self.process.pid, signal.SIGKILL)
        self.process.wait()


def start_viewer(args, directory):
    """The viewer, its output read by the test, in `directory`."""
    return subprocess.Popen([VIEWER, *args], cwd=directory, env=DISPLAY.env, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True)


def reports(stdout):
    """The viewer's report lines, as (t, paints, updates, last_seq)."""
    return [tuple(int(g) for g in m.groups()) for m in map(REPORT.match, stdout.splitlines()) if m]


def read_context(path):
    with open(path) as file:
        return json.load(file)


def the_log(directory):
    """The one log in `directory`'s logs, its lines without their times, and its path."""
    logs_dir = os.path.join(directory, "logs")
    logs = [name for name in os.listdir(logs_dir) if re.fullmatch(r"viewer-\d{8}-\d{6}-\d+\.log", name)]
    assert len(logs) == 1, logs
    path = os.path.join(logs_dir, logs[0])
    with open(path) as file:
        text = file.read()
    lines = [re.sub(r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z \+\d+\.\d{3}s ", "", line) for line in text.splitlines()]
    return lines, path


def sanitized(lines):
    """Whether the log says the viewer was built with the sanitizers."""
    return any(line.startswith("build:") and "AddressSanitizer" in line for line in lines)


class ViewerOnTheExchangeTest(unittest.TestCase):
    def test_it_takes_in_the_market_data_paints_reports_and_exits_after_its_time(self):
        needs_display(self)
        self.assertFalse(order_port_open(), f"something is already listening on this test's order port, {ORDER_PORT}")
        with tempfile.TemporaryDirectory() as directory:
            context, screenshot = os.path.join(directory, "context.json"), os.path.join(directory, "last.ppm")
            exchange = Process([EXCHANGE, *ENDPOINTS], directory)
            client = None
            try:
                deadline = time.monotonic() + START_LIMIT
                while not order_port_open():
                    self.assertTrue(exchange.running(), "the exchange exited")
                    self.assertLess(time.monotonic(), deadline, f"the exchange didn't open port {ORDER_PORT}")
                    time.sleep(0.1)
                # The viewer joins first, so it sees the first update; the client trades a second
                # or two after it starts.
                viewer = start_viewer(["--exit-after", "6", *STREAMS, "--context-file", context, "--screenshot", screenshot],
                                      directory)
                client = Process([TRADING, "5", "RANDOM", "--warm-up-seconds", "1", *ENDPOINTS], directory)
                stdout, stderr = viewer.communicate(timeout=START_LIMIT + 6)
            finally:
                if client is not None:
                    client.kill()
                exchange.kill()

            self.assertEqual(viewer.returncode, 0, stderr)
            lines = reports(stdout)
            # One line a second, t=1 to t=6: the last paint, at 6 seconds, reports too.
            self.assertEqual([t for t, *_ in lines], [1, 2, 3, 4, 5, 6], stdout)
            # About 60 paints a second, whatever the display does: the loop paces itself.
            paints = [p for _, p, *_ in lines]
            per_second = (paints[-1] - paints[0]) / (len(paints) - 1)
            self.assertLessEqual(per_second, 66, f"{per_second:.0f} paints a second: the loop doesn't pace itself")
            self.assertGreaterEqual(per_second, 15, f"only {per_second:.0f} paints a second")
            # The market data arrived and was taken in, in order, nothing missed.
            updates = lines[-1][2]
            self.assertGreater(updates, 0, "no market data reached the viewer")
            self.assertEqual(lines[-1][3], updates, "the books aren't as of the last update received")

            # The last context file: from this viewer, no longer running, every update counted.
            final = read_context(context)
            self.assertEqual(final["writer"]["pid"], viewer.pid)
            self.assertFalse(final["writer"]["running"])
            feed = final["background"]["feed"]
            self.assertEqual(feed["status"], "live")
            self.assertEqual(feed["recoveryCount"], 0)
            self.assertGreaterEqual(feed["updatesReceived"], updates)
            self.assertEqual(feed["lastSeq"], feed["updatesReceived"])

            # The last frame, as a PPM the window's size: not blank.
            with open(screenshot, "rb") as file:
                image = file.read()
            header = re.match(rb"P6\n(\d+) (\d+)\n255\n", image)
            self.assertIsNotNone(header, image[:20])
            width, height = int(header.group(1)), int(header.group(2))
            self.assertGreater(width * height, 0)
            pixels = image[header.end():]
            self.assertEqual(len(pixels), width * height * 3)
            self.assertGreater(len(set(pixels)), 2, "the screenshot is blank")

            # Its log: how it started, the feed, each second's line, and how it ended.
            log, _ = the_log(directory)
            self.assertTrue(log[0].startswith("start: viewer --exit-after 6 "), log[0])
            self.assertTrue(any(line.startswith("build: GCC ") for line in log), log)
            self.assertTrue(any(line.startswith("binary: ") for line in log), log)
            self.assertTrue(any(line.startswith("crash recording: ") for line in log), log)
            self.assertIn(f"live: joined the incremental updates at {INCREMENTAL} and the snapshots at {SNAPSHOT}", log)
            self.assertTrue(any(line.startswith("feed: live, the books as of update ") for line in log), log)
            for t, paints_then, updates_then, last in lines:
                self.assertIn(f"t={t} paints={paints_then} updates={updates_then} books as of update {last}", log)
            self.assertIn(f"screenshot: {screenshot}", " ".join(log))
            self.assertEqual(log[-3:-1], ["stopping: --exit-after 6 s",
                                         f"the final context is published to {context}, saying this viewer has stopped"])
            self.assertEqual(log[-1], "end: --exit-after 6 s; exit code 0")


class ViewerEndsOnASignalTest(unittest.TestCase):
    def ends_on(self, signum):
        needs_display(self)
        with tempfile.TemporaryDirectory() as directory:
            context = os.path.join(directory, "context.json")
            viewer = start_viewer([*STREAMS, "--context-file", context], directory)
            try:
                # Once it has painted for a second: its first report line.
                line = viewer.stdout.readline()
                self.assertRegex(line, REPORT, viewer.stderr.read() if not line else line)
                viewer.send_signal(signum)
                stdout, stderr = viewer.communicate(timeout=START_LIMIT)
            finally:
                if viewer.poll() is None:
                    viewer.kill()
                    viewer.wait()
            self.assertEqual(viewer.returncode, 0, stderr)
            self.assertIn("viewer: stopping, as a signal asked", stderr)
            self.assertFalse(read_context(context)["writer"]["running"])
            log, _ = the_log(directory)
            name = signal.Signals(signum).name
            self.assertIn(f"stopping: {name}", log)
            self.assertEqual(log[-1], f"end: {name}; exit code 0")

    def test_sigterm_ends_it_as_closing_the_window_would(self):
        self.ends_on(signal.SIGTERM)

    def test_sigint_ends_it_as_closing_the_window_would(self):
        self.ends_on(signal.SIGINT)


class ViewerDiesTest(unittest.TestCase):
    """Ended without warning, the viewer's log still has everything it wrote before."""

    def run_until_reporting(self, directory):
        viewer = start_viewer([*STREAMS, "--context-file", os.path.join(directory, "context.json")], directory)
        line = viewer.stdout.readline()
        if not REPORT.match(line):
            viewer.kill()
            self.fail(viewer.communicate()[1])
        return viewer

    def test_killed_outright_its_log_has_everything_until_then(self):
        needs_display(self)
        with tempfile.TemporaryDirectory() as directory:
            viewer = self.run_until_reporting(directory)
            viewer.send_signal(signal.SIGKILL)
            viewer.communicate(timeout=START_LIMIT)
            self.assertEqual(viewer.returncode, -signal.SIGKILL)
            log, _ = the_log(directory)
            self.assertTrue(log[0].startswith("start: viewer "), log)
            self.assertTrue(any(REPORT.match("viewer: " + line) for line in log), log)  # the first second's line
            self.assertFalse(any(line.startswith("end:") for line in log), log)

    def test_a_crash_is_recorded(self):
        needs_display(self)
        with tempfile.TemporaryDirectory() as directory:
            viewer = self.run_until_reporting(directory)
            viewer.send_signal(signal.SIGSEGV)
            viewer.communicate(timeout=START_LIMIT)
            log, path = the_log(directory)
            if sanitized(log):
                # AddressSanitizer reports a segmentation fault itself, to the file beside the log.
                self.assertEqual(viewer.returncode, 1)
                reports = [name for name in os.listdir(os.path.dirname(path))
                           if name.startswith(os.path.basename(path) + ".sanitizer.")]
                self.assertEqual(len(reports), 1, os.listdir(os.path.dirname(path)))
                with open(os.path.join(os.path.dirname(path), reports[0])) as file:
                    self.assertIn("AddressSanitizer: SEGV", file.read())
            else:
                # Recorded with a backtrace, and the signal still ends the process.
                self.assertEqual(viewer.returncode, -signal.SIGSEGV)
                self.assertIn("crash: fatal signal 11 (SIGSEGV); the backtrace, innermost first:", log)
                self.assertEqual(log[-1], "crash: end of the backtrace; the signal now ends the process")


class ViewerRefusesTest(unittest.TestCase):
    """Refused before any window opens, so these need no display."""

    def test_a_bad_command_line_is_refused_with_its_usage(self):
        with tempfile.TemporaryDirectory() as directory:
            result = subprocess.run([VIEWER, "--bogus"], cwd=directory, capture_output=True, text=True, timeout=30)
            # Before any log: where it would go is part of what couldn't be read.
            self.assertFalse(os.path.exists(os.path.join(directory, ".fillscope")))
        self.assertEqual(result.returncode, 2)
        self.assertIn("usage: viewer", result.stderr)

    def test_a_replay_it_cant_read_is_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            result = subprocess.run([VIEWER, "--replay", os.path.join(directory, "missing.dat")], cwd=directory,
                                    capture_output=True, text=True, timeout=30)
            # Its log is beside the default context file, .fillscope/context.json.
            log, _ = the_log(os.path.join(directory, ".fillscope"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("missing.dat", result.stderr)
        self.assertTrue(any("missing.dat" in line for line in log), log)
        self.assertEqual(log[-1], "end: the replay can't be read; exit code 1")


if __name__ == "__main__":
    DISPLAY = Display()
    try:
        result = unittest.main(argv=sys.argv[:1], exit=False).result
    finally:
        DISPLAY.close()
    if not result.wasSuccessful():
        sys.exit(1)
    sys.exit(77 if DISPLAY.env is None else 0)  # without a display, the window's tests were skipped
