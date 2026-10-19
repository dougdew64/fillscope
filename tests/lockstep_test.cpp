// The debugger anchor and the debugger check.

#include "lockstep/anchor.h"

#include <gtest/gtest.h>

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
}

TEST(DebuggerAttached, FalseWhenRunWithoutADebugger) {
    // This test process runs under CTest, not a debugger.
    EXPECT_FALSE(debugger_attached());
}

TEST(Anchor, RecordsEachFrameIndex) {
    fillscope_frame_anchor(7);
    EXPECT_EQ(last_anchored_frame(), 7u);
    fillscope_frame_anchor(8);
    EXPECT_EQ(last_anchored_frame(), 8u);
}

TEST(Anchor, SelfStopWithoutADebuggerDoesNotStop) {
    // If the anchor executed its breakpoint instruction here, SIGTRAP would kill this process and
    // the test would fail as a crash.
    set_self_stop(true);
    fillscope_frame_anchor(9);
    set_self_stop(false);
    EXPECT_EQ(last_anchored_frame(), 9u);
}

}  // namespace
}  // namespace fillscope
