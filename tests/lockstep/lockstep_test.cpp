// The debugger anchor and the tracer check.

#include "lockstep/anchor.h"

#include <gtest/gtest.h>

#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cstdlib>

namespace fillscope {
namespace {

TEST(TracerAttached, ReadsTracerPid) {
    EXPECT_FALSE(tracer_attached("Name:\tframe_source\nState:\tR (running)\nTracerPid:\t0\nUid:\t1000\n"));
    EXPECT_TRUE(tracer_attached("Name:\tframe_source\nTracerPid:\t4321\nUid:\t1000\n"));
}

TEST(TracerAttached, MissingOrMalformedMeansNoTracer) {
    EXPECT_FALSE(tracer_attached(""));
    EXPECT_FALSE(tracer_attached("Name:\tframe_source\nUid:\t1000\n"));
    EXPECT_FALSE(tracer_attached("TracerPid:\tnone\n"));
    EXPECT_FALSE(tracer_attached("TracerPid:\t12x\n"));
}

TEST(TracerAttached, OnlyALineStartingWithTheKeyCounts) {
    // A process can name itself anything, and its name is the status file's first line.
    EXPECT_FALSE(tracer_attached("Name:\tTracerPid: 77\nTracerPid:\t0\n"));
}

// Skips a test that needs this process untraced, as under CTest, when it is being traced (run under
// GDB or strace, say), where it would fail, or (with self-stop) end the process.
#define REQUIRE_UNTRACED()                                                   \
    do {                                                                     \
        if (const auto traced = tracer_attached_now(); traced && *traced) { \
            GTEST_SKIP() << "this process is being traced";                  \
        }                                                                    \
    } while (false)

TEST(TracerAttachedNow, ReadsThisProcessesStatus) {
    REQUIRE_UNTRACED();
    const auto traced = tracer_attached_now();
    ASSERT_TRUE(traced.has_value()) << traced.error().message();
    EXPECT_FALSE(*traced);
}

// The anchor's state is process-wide, and GoogleTest runs these in one process (CTest runs each in
// its own, but a whole-program run doesn't), so each test checks only what it set.
TEST(Anchor, HasRecordedNothingBeforeItsFirstCall) {
    // In a process of its own (a death test re-runs this program for just this statement), since
    // the other tests here call the anchor.
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(std::exit(last_anchored_frame().has_value() ? 1 : 0), ::testing::ExitedWithCode(0), "");
}

TEST(Anchor, RecordsEachFrameIndex) {
    fillscope_frame_anchor(7);
    EXPECT_EQ(last_anchored_frame(), 7u);
    fillscope_frame_anchor(0);
    EXPECT_EQ(last_anchored_frame(), 0u) << "frame 0 is a frame, not 'none'";
}

TEST(Anchor, SelfStopWithoutATracerDoesNotStop) {
    // If the anchor executed its breakpoint instruction here, SIGTRAP would kill this process and
    // the test would fail as a crash.
    REQUIRE_UNTRACED();
    set_self_stop(true);
    fillscope_frame_anchor(9);
    set_self_stop(false);
    EXPECT_EQ(last_anchored_frame(), 9u);
}

// What a traced child does at the anchor, seen by its tracer (this test, standing in for a
// debugger): whether it stopped there with SIGTRAP, and how it exited.
struct TracedRun {
    bool stopped_by_sigtrap = false;
    int exit_status = -1;
};

TracedRun run_anchor_traced(bool self_stop) {
    const pid_t child = ::fork();
    if (child == 0) {
        // The child asks to be traced by its parent, then stops so the parent can wait for it.
        ::ptrace(PTRACE_TRACEME, 0, nullptr, nullptr);
        ::raise(SIGSTOP);
        set_self_stop(self_stop);
        fillscope_frame_anchor(5);
        ::_exit(last_anchored_frame() == 5u ? 0 : 1);
    }
    TracedRun run;
    int status = 0;
    ::waitpid(child, &status, 0);  // stopped by its own SIGSTOP
    ::ptrace(PTRACE_CONT, child, nullptr, nullptr);
    ::waitpid(child, &status, 0);
    if (WIFSTOPPED(status) && WSTOPSIG(status) == SIGTRAP) {
        run.stopped_by_sigtrap = true;
        ::ptrace(PTRACE_CONT, child, nullptr, nullptr);  // as Continue does: the signal is not passed on
        ::waitpid(child, &status, 0);
    }
    run.exit_status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return run;
}

// Self-stop without a debugger: the test traces a child process itself, so a self-stop that does
// nothing (or stops when it shouldn't) fails here, not only in the slower debugger tests.
TEST(Anchor, SelfStopStopsATracedProcessInTheAnchor) {
    const TracedRun run = run_anchor_traced(true);
    EXPECT_TRUE(run.stopped_by_sigtrap);
    EXPECT_EQ(run.exit_status, 0) << "Continue runs on, and the anchor recorded the frame";
}

TEST(Anchor, WithoutSelfStopATracedProcessRunsThrough) {
    const TracedRun run = run_anchor_traced(false);
    EXPECT_FALSE(run.stopped_by_sigtrap);
    EXPECT_EQ(run.exit_status, 0);
}

}  // namespace
}  // namespace fillscope
