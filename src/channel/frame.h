// A frame: the state after one event, sent from the process that produces it to the viewer. For now
// a stand-in (a random-walk price) until the message interfaces exist.
#pragma once

#include <cstdint>
#include <type_traits>

namespace fillscope {

struct Frame {
    std::uint64_t index;  // 0, 1, 2, ... in the order the frames were produced
    std::int64_t price;
    std::uint32_t qty;
    std::uint32_t reserved;

    friend bool operator==(const Frame&, const Frame&) = default;
};

// Sent as raw bytes over a socket, so its layout must be fixed and copyable as bytes.
static_assert(std::is_trivially_copyable_v<Frame>);
static_assert(sizeof(Frame) == 24);

}  // namespace fillscope
