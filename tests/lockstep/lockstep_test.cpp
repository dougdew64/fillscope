// The debugger anchor and the tracer check.

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
    EXPECT_FALSE(tracer_attached("TracerPid:\t12x\n"));
}

TEST(TracerAttached, OnlyALineStartingWithTheKeyCounts) {
    // A process can name itself anything, and its name is the status file's first line.
    EXPECT_FALSE(tracer_attached("Name:\tTracerPid: 77\nTracerPid:\t0\n"));
}

TEST(TracerAttachedNow, FalseWhenRunWithoutATracer) {
    // This test process runs under CTest, not a debugger.
    EXPECT_FALSE(tracer_attached_now());
}

// The anchor's state is process-wide, and GoogleTest runs these in one process (CTest runs each in
// its own, but a whole-program run doesn't), so each test checks only what it set.
TEST(Anchor, RecordsEachFrameIndex) {
    fillscope_frame_anchor(7);
    EXPECT_EQ(last_anchored_frame(), 7u);
    fillscope_frame_anchor(0);
    EXPECT_EQ(last_anchored_frame(), 0u) << "frame 0 is a frame, not 'none'";
}

TEST(Anchor, SelfStopWithoutATracerDoesNotStop) {
    // If the anchor executed its breakpoint instruction here, SIGTRAP would kill this process and
    // the test would fail as a crash.
    set_self_stop(true);
    fillscope_frame_anchor(9);
    set_self_stop(false);
    EXPECT_EQ(last_anchored_frame(), 9u);
}

}  // namespace
}  // namespace fillscope
