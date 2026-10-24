# Runs exchange_replay under LLDB through its Python API, synchronously, continuing through every
# stop, while acting as the viewer: it listens to the multicast group the replay publishes on, and at
# each stop counts the market updates that have arrived. Prints one line per stop and one at exit,
# for replay_debugger_test.cpp to check:
#   stop: signal SIGTRAP in fillscope_request_anchor request_number=2 updates_published=3 file=lockstep_anchor.cpp line=104 received=3
#   exchange_replay: 3 requests, 4 market updates, published to 233.252.18.1:24001
#   exited: 0
# (the program's own output, then its exit status). "received" is how many updates had arrived when the replay stopped; "updates_published" how many
# it says it had sent. The lockstep claim: every update of request N is sent before its stop.
#
#   python3 -I replay_lldb_driver.py [--break-by-name] GROUP PORT PROGRAM [ARGS...]
#
# LLDB's batch mode treats a stop by signal as a crash and runs no further commands, which is why
# this drives LLDB itself. LLDB's Python module lives wherever `lldb -P` says.
import socket
import struct
import subprocess
import sys
import time

sys.path.insert(0, subprocess.run(["lldb", "-P"], capture_output=True, text=True, check=True).stdout.strip())
import lldb  # noqa: E402 (imported once the path is known)

MAX_STOPS = 1000  # a runaway guard, far above any test's request count
UPDATE_SIZE = 42  # one MDPMarketUpdate's bytes


class Viewer:
    """A member of the group, as the viewer joins it: bound to the group's address and port."""

    def __init__(self, group, port):
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
        self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.socket.bind((group, port))
        membership = struct.pack("4s4s", socket.inet_aton(group), socket.inet_aton("0.0.0.0"))
        self.socket.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, membership)
        self.socket.setblocking(False)
        self.received = 0

    def count(self, expected):
        """Takes what has arrived and returns how many updates have arrived in all. At a stop the
        replay is frozen, but multicast looped back to this machine is delivered asynchronously, so
        it waits up to two seconds for `expected` (generous, for a loaded machine and the sanitizer
        build). A replay that sent nothing before the stop falls short however long it waits."""
        deadline = time.monotonic() + 2.0
        while True:
            try:
                self.received += len(self.socket.recv(65536)) // UPDATE_SIZE
                continue
            except BlockingIOError:
                pass
            if self.received >= expected or time.monotonic() > deadline:
                return self.received
            time.sleep(0.001)


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
    viewer = Viewer(arguments[0], int(arguments[1]))
    debugger = lldb.SBDebugger.Create()
    debugger.SetAsync(False)
    target = debugger.CreateTarget(arguments[2])
    if by_name:
        target.BreakpointCreateByName("fillscope_request_anchor")
    process = target.LaunchSimple(arguments[3:], None, ".")
    for _ in range(MAX_STOPS):
        if process.GetState() == lldb.eStateExited:
            # What the program printed, which under LLDB goes to LLDB rather than to this script's
            # output: the replay's closing line says it ran to its end.
            while output := process.GetSTDOUT(4096):
                print(output, end="")
            print(f"exited: {process.GetExitStatus()}")
            return
        thread = process.GetSelectedThread()
        frame = thread.GetFrameAtIndex(0)
        line = frame.GetLineEntry()
        published = frame.FindVariable("updates_published").GetValue()
        expected = int(published) if published is not None else 0
        print(f"stop: {describe_stop(process, thread)} in {frame.GetFunctionName()} "
              f"request_number={frame.FindVariable('request_number').GetValue()} updates_published={published} "
              f"file={line.GetFileSpec().GetFilename()} line={line.GetLine()} received={viewer.count(expected)}")
        process.Continue()
    print("too many stops")
    process.Kill()


main()
