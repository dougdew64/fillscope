// The messages a frame source sends the viewer, and how each is encoded on the socket.
//
// There are two kinds, held in a std::variant (a type-safe union: a Message is exactly one of its
// alternatives at a time, and the compiler checks that code handling it covers each one):
//   - SourceHello opens every connection and describes the run the frames that follow come from.
//     With its seed, a frame index becomes a replay address: the frame source produces the same
//     frames from the same seed (tools/frame_source/frame_source.h says how it guarantees that),
//     so (seed, index) identifies one exact state that can be rebuilt.
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

using Message = std::variant<SourceHello, Frame>;

// What may cross the socket as raw bytes: a type whose bytes are exactly its value (no pointers,
// no padding). A C++20 concept names a compile-time requirement, so a template constrained by it
// rejects any other type with this reason rather than with an error deep in its body.
template <typename T>
concept WirePayload = std::has_unique_object_representations_v<T>;

static_assert(WirePayload<SourceHello> && sizeof(SourceHello) == 16);
static_assert(WirePayload<Frame>);

// A helper for std::visit: builds one callable object out of several lambdas, one per alternative,
// so a visit reads as a list of cases. `Overloaded{[](const A&) {...}, [](const B&) {...}}` derives
// from both lambda types and brings both call operators into scope; overload resolution then picks
// the one matching the variant's current alternative. (A C++17 idiom; since C++20 no deduction
// guide is needed, because aggregates deduce their template arguments.)
template <typename... Lambdas>
struct Overloaded : Lambdas... {
    using Lambdas::operator()...;
};

// On the socket each message is one SOCK_SEQPACKET datagram: a 4-byte kind, 4 unused bytes, then
// the payload's bytes at offset 8. Both ends run on the same machine, so the bytes are in its
// native byte order. Encoding and decoding copy with std::memcpy, so the payload's alignment in
// the buffer doesn't matter.
enum class MessageKind : std::uint32_t {
    source_hello = 1,
    frame = 2,
};

inline constexpr std::size_t kMessageHeaderSize = 8;

// The largest payload among Message's alternatives, computed from the variant itself, so a new
// alternative can never be larger than the buffers sized from it. A partial specialization
// matches std::variant<Ts...> and exposes the pack Ts; std::max over an initializer list takes
// the largest sizeof among them.
template <typename Variant>
struct LargestAlternative;
template <typename... Alternatives>
struct LargestAlternative<std::variant<Alternatives...>> {
    static constexpr std::size_t size = std::max({sizeof(Alternatives)...});
};

inline constexpr std::size_t kMaxMessageSize = kMessageHeaderSize + LargestAlternative<Message>::size;

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
