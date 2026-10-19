#include "channel/message.h"

#include <cstring>
#include <format>

namespace fillscope {
namespace {

// Writes the header and a trivially copyable payload into `out`.
template <typename Payload>
void encode_as(MessageKind kind, const Payload& payload, EncodedMessage& out) {
    const auto kind_value = static_cast<std::uint32_t>(kind);
    std::memcpy(out.bytes.data(), &kind_value, sizeof kind_value);
    std::memcpy(out.bytes.data() + kMessageHeaderSize, &payload, sizeof payload);
    out.size = kMessageHeaderSize + sizeof payload;
}

// Reads a trivially copyable payload after the header, if the datagram is exactly its size.
// memcpy into a properly aligned local object is the well-defined way to read an object's bytes
// from a buffer; casting the buffer's pointer to Payload* would break C++'s aliasing rules.
template <typename Payload>
std::expected<Message, std::string> decode_as(std::span<const std::byte> datagram, const char* name) {
    if (datagram.size() != kMessageHeaderSize + sizeof(Payload)) {
        return std::unexpected{std::format("a {} message of {} bytes; expected {}", name, datagram.size(),
                                           kMessageHeaderSize + sizeof(Payload))};
    }
    Payload payload;
    std::memcpy(&payload, datagram.data() + kMessageHeaderSize, sizeof payload);
    return Message{payload};
}

}  // namespace

EncodedMessage encode(const Message& message) {
    EncodedMessage out;
    // std::visit calls the lambda that matches the alternative the variant holds.
    std::visit(Overloaded{
                   [&](const SourceHello& hello) { encode_as(MessageKind::source_hello, hello, out); },
                   [&](const Frame& frame) { encode_as(MessageKind::frame, frame, out); },
               },
               message);
    return out;
}

std::expected<Message, std::string> decode(std::span<const std::byte> datagram) {
    if (datagram.size() < kMessageHeaderSize) {
        return std::unexpected{std::format("a message of {} bytes is shorter than its header", datagram.size())};
    }
    std::uint32_t kind_value = 0;
    std::memcpy(&kind_value, datagram.data(), sizeof kind_value);
    switch (static_cast<MessageKind>(kind_value)) {
        case MessageKind::source_hello:
            return decode_as<SourceHello>(datagram, "source hello");
        case MessageKind::frame:
            return decode_as<Frame>(datagram, "frame");
    }
    // Not inside the switch's default: with no default, the compiler warns (-Wswitch) if a new
    // MessageKind is added and not handled above.
    return std::unexpected{std::format("unknown message kind {}", kind_value)};
}

}  // namespace fillscope
