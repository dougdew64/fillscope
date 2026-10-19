# Runs a program under LLDB through its Python API, synchronously, continuing through every stop.
# Prints one line per stop and one at exit, for lockstep_debugger_test.cpp to check:
#   stop: signal SIGTRAP in ::fillscope_frame_anchor(uint64_t) frame_index=0 file=anchor.cpp
#   exited: 0
#
#   python3 -I lldb_self_stop.py PROGRAM [ARGS...]
#
# LLDB's Python module lives wherever `lldb -P` says. The script adds that to its own search path:
# with -I (isolated mode), Python ignores PYTHONPATH, so it can't be passed that way.
import subprocess
import sys

sys.path.insert(0, subprocess.run(["lldb", "-P"], capture_output=True, text=True, check=True).stdout.strip())
import lldb  # noqa: E402 (imported once the path is known)

MAX_STOPS = 1000  # a runaway guard, far above any test's frame count


def describe_stop(process, thread):
    """The stop's reason, naming the actual signal for a stop by signal."""
    if thread.GetStopReason() == lldb.eStopReasonSignal:
        number = thread.GetStopReasonDataAtIndex(0)
        return f"signal {process.GetUnixSignals().GetSignalAsCString(number)}"
    return thread.GetStopDescription(100)


def main():
    debugger = lldb.SBDebugger.Create()
    debugger.SetAsync(False)
    target = debugger.CreateTarget(sys.argv[1])
    process = target.LaunchSimple(sys.argv[2:], None, ".")
    for _ in range(MAX_STOPS):
        if process.GetState() == lldb.eStateExited:
            print(f"exited: {process.GetExitStatus()}")
            return
        thread = process.GetSelectedThread()
        frame = thread.GetFrameAtIndex(0)
        print(f"stop: {describe_stop(process, thread)} in {frame.GetFunctionName()} "
              f"frame_index={frame.FindVariable('frame_index').GetValue()} "
              f"file={frame.GetLineEntry().GetFileSpec().GetFilename()}")
        process.Continue()
    print("too many stops")
    process.Kill()


main()
