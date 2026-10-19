// The one message the engine sends the viewer: the state after one event.
#pragma once

#include <cstdint>
#include <string_view>

namespace lockstep {

// A Linux abstract socket (leading '\0'): no file to create or clean up.
inline constexpr std::string_view kSocketName{"\0fillscope-lockstep", 19};

struct Frame {
    std::uint64_t index;  // 0, 1, 2, ... in the order the engine produced them
    std::int64_t price;   // a random walk, standing in for book state
    std::uint32_t qty;
    std::uint32_t reserved;
};

}  // namespace lockstep
