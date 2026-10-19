# Runs a frame source under LLDB through its Python API, synchronously, continuing through every
# stop, while acting as its viewer: it listens on the socket, accepts and acknowledges the frame
# source's connection, and at each stop counts the messages that have arrived. Prints one line per
# stop and one at exit, for lockstep_debugger_test.cpp to check:
#   stop: signal SIGTRAP in ::fillscope_frame_anchor(uint64_t) frame_index=0 file=anchor.cpp line=128 received=2
#   exited: 0
# "received" is how many messages the frame source had sent when it stopped: its run's description
# plus frames 0 to frame_index if every frame is sent before its stop, which is the lockstep claim.
#
#   python3 -I lldb_driver.py [--break-by-name] SOCKET PROGRAM [ARGS...]
#
# LLDB's batch mode treats a stop by signal as a crash and runs no further commands, which is why
# this drives LLDB itself. LLDB's Python module lives wherever `lldb -P` says; the script adds that
# to its own search path, since with -I (isolated mode) Python ignores PYTHONPATH.
import socket
import subprocess
import sys
import threading

sys.path.insert(0, subprocess.run(["lldb", "-P"], capture_output=True, text=True, check=True).stdout.strip())
import lldb  # noqa: E402 (imported once the path is known)

MAX_STOPS = 1000  # a runaway guard, far above any test's frame count


class Viewer:
    """The listening end of the frame source's socket. Accepts on a thread of its own, because
    LLDB's launch blocks until the first stop, and the frame source waits to be accepted before
    it sends anything."""

    def __init__(self, name):
        self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.listener.bind("\0" + name)  # the abstract namespace, as src/channel/net.cpp uses
        self.listener.listen(1)
        self.connection = None
        self.received = 0
        self.thread = threading.Thread(target=self.accept, daemon=True)
        self.thread.start()

    def accept(self):
        connection, _ = self.listener.accept()
        connection.send(b"\x01")  # the acknowledgement the frame source waits for
        self.connection = connection

    def count(self):
        """Takes every message waiting and returns how many have arrived in all. At a stop the
        frame source is frozen, so what it has sent is all waiting."""
        if self.connection is None:
            return self.received
        while True:
            try:
                if not self.connection.recv(64, socket.MSG_DONTWAIT):
                    break  # the frame source closed its end
                self.received += 1
            except BlockingIOError:
                break
        return self.received


def describe_stop(process, thread):
    """The stop's reason, naming the actual signal for a stop by signal."""
    if thread.GetStopReason() == lldb.eStopReasonSignal:
        number = thread.GetStopReasonDataAtIndex(0)
        return f"signal {process.GetUnixSignals().GetSignalAsCString(number)}"
    return thread.GetStopDescription(100)


def main():
    arguments = sys.argv[1:]
    by_name = arguments[:1] == ["--break-by-name"]
    if by_name:
        arguments = arguments[1:]
    viewer = Viewer(arguments[0])
    debugger = lldb.SBDebugger.Create()
    debugger.SetAsync(False)
    target = debugger.CreateTarget(arguments[1])
    if by_name:
        target.BreakpointCreateByName("fillscope_frame_anchor")
    process = target.LaunchSimple(arguments[2:], None, ".")
    for _ in range(MAX_STOPS):
        if process.GetState() == lldb.eStateExited:
            print(f"exited: {process.GetExitStatus()}")
            return
        thread = process.GetSelectedThread()
        frame = thread.GetFrameAtIndex(0)
        line = frame.GetLineEntry()
        print(f"stop: {describe_stop(process, thread)} in {frame.GetFunctionName()} "
              f"frame_index={frame.FindVariable('frame_index').GetValue()} "
              f"file={line.GetFileSpec().GetFilename()} line={line.GetLine()} received={viewer.count()}")
        process.Continue()
    print("too many stops")
    process.Kill()


main()
