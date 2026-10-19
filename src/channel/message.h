// The messages a frame source sends the viewer, and how each is encoded on the socket.
//
// There are two kinds, held in a std::variant (a type-safe union: a Message is exactly one of its
// alternatives at a time, and the compiler checks that code handling it covers each one):
//   - SourceHello opens every connection and describes the run the frames that follow come from.
//     With its seed, a frame index becomes a replay address: the same seed always produces the
//     same frames, so (seed, index) identifies one exact state that can be rebuilt.
//   - Frame carries one frame (channel/frame.h).
#pragma once

#include "channel/frame.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <type_traits>
#include <variant>

namespace fillscope {

struct SourceHello {
    std::uint64_t seed;         // the source's random seed: the same seed gives the same frames
    std::uint64_t frame_count;  // how many frames the source will send in this run

    friend bool operator==(const SourceHello&, const SourceHello&) = default;
};
static_assert(std::is_trivially_copyable_v<SourceHello>);

using Message = std::variant<SourceHello, Frame>;

// A helper for std::visit: builds one callable object out of several lambdas, one per alternative,
// so a visit reads as a list of cases. `Overloaded{[](const A&) {...}, [](const B&) {...}}` derives
// from both lambda types and brings both call operators into scope; overload resolution then picks
// the one matching the variant's current alternative. (A C++17 idiom; since C++20 no deduction
// guide is needed, because aggregates deduce their template arguments.)
template <typename... Lambdas>
struct Overloaded : Lambdas... {
    using Lambdas::operator()...;
};

// On the socket each message is one SOCK_SEQPACKET datagram: a 4-byte kind, 4 bytes of padding so
// the payload starts 8-byte aligned, then the payload's bytes. Both ends run on the same machine,
// so the bytes are in its native byte order.
enum class MessageKind : std::uint32_t {
    source_hello = 1,
    frame = 2,
};

inline constexpr std::size_t kMessageHeaderSize = 8;
inline constexpr std::size_t kMaxMessageSize = kMessageHeaderSize + std::max(sizeof(SourceHello), sizeof(Frame));

// One encoded message: a fixed-size buffer and how much of it is used. A plain value type, so
// encoding never allocates.
struct EncodedMessage {
    std::array<std::byte, kMaxMessageSize> bytes{};
    std::size_t size = 0;

    [[nodiscard]] std::span<const std::byte> view() const noexcept { return {bytes.data(), size}; }
};

[[nodiscard]] EncodedMessage encode(const Message& message);

// The message in one datagram, or why it isn't one: too short, an unknown kind, or a size that
// doesn't match its kind. std::expected (C++23) carries either the value or the error, and the
// caller must look to find out which.
[[nodiscard]] std::expected<Message, std::string> decode(std::span<const std::byte> datagram);

}  // namespace fillscope
