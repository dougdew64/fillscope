# Runs a program under LLDB through its Python API, synchronously, continuing through every stop.
# Prints one line per stop and one at exit, for lockstep_debugger_test.cpp to check:
#   stop: SIGTRAP in ::fillscope_frame_anchor(uint64_t) frame_index=0 file=anchor.cpp
#   exited: 0
#
#   python3 -I lldb_self_stop.py PROGRAM [ARGS...]     (with PYTHONPATH from `lldb -P`)
import sys

import lldb

MAX_STOPS = 1000  # a runaway guard, far above any test's frame count


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
        reason = "SIGTRAP" if thread.GetStopReason() == lldb.eStopReasonSignal else thread.GetStopDescription(100)
        print(f"stop: {reason} in {frame.GetFunctionName()} "
              f"frame_index={frame.FindVariable('frame_index').GetValue()} "
              f"file={frame.GetLineEntry().GetFileSpec().GetFilename()}")
        process.Continue()
    print("too many stops")
    process.Kill()


main()
