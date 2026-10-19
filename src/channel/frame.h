// A frame: the state after one event, sent from the process that produces it to the viewer.
//
// For now this is a stand-in, used to build and test the viewer before the trading domain's real
// messages are designed. Its fields are placeholders: `price` is a random walk, and `id` is a
// recurring identity (standing in for something like an order id) so the viewer has something to
// follow from frame to frame. None of it is a draft of the real message interfaces.
#pragma once

#include <cstdint>
#include <type_traits>

namespace fillscope {

struct Frame {
    std::uint64_t index;  // 0, 1, 2, ... in the order the frames were produced within one run
    std::int64_t price;   // a stand-in value that changes from frame to frame
    std::uint32_t qty;    // a stand-in quantity
    std::uint32_t id;     // a stand-in identity that recurs across frames, so it can be followed

    // C++20 defaulted comparison: the compiler writes a memberwise ==, and != follows from it.
    friend bool operator==(const Frame&, const Frame&) = default;
};

// A Frame crosses the socket as raw bytes (channel/message.h), which is only meaningful if its
// bytes are exactly its value. has_unique_object_representations is the exact test: trivially
// copyable *and* no padding bytes, so two equal Frames have identical bytes. (Trivially copyable
// alone would allow padding, whose bytes are indeterminate.) If a later edit broke this, the build
// fails here instead of frames being corrupted at run time.
static_assert(std::has_unique_object_representations_v<Frame>);
static_assert(sizeof(Frame) == 24);

}  // namespace fillscope
