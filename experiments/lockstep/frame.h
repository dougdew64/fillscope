// The one message the engine sends the viewer: the state after one event.
#pragma once

#include <cstdint>
#include <type_traits>

namespace lockstep {

struct Frame {
    std::uint64_t index;  // 0, 1, 2, ... in the order the engine produced them
    std::int64_t price;   // a random walk, standing in for book state
    std::uint32_t qty;
    std::uint32_t reserved;

    friend bool operator==(const Frame&, const Frame&) = default;
};

// Sent as raw bytes over a socket, so its layout must be fixed and copyable as bytes.
static_assert(std::is_trivially_copyable_v<Frame>);
static_assert(sizeof(Frame) == 24);

}  // namespace lockstep
