// The socket that carries messages (channel/message.h) from a frame source to the viewer: a Unix
// SOCK_SEQPACKET socket in Linux's abstract namespace. SEQPACKET keeps message boundaries (each
// send arrives as one whole datagram) and delivers in order; the abstract namespace means there is
// no socket file on disk to clean up.
//
// Failures are returned, not thrown: as std::expected<..., std::error_code>, where the
// std::error_code carries the operating system's reason (errno) and can describe itself with
// .message(), so a caller can report exactly what went wrong.
#pragma once

#include "channel/message.h"

#include <chrono>
#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fillscope {

// Socket names are given without the abstract namespace's leading NUL; the functions add it. A
// name may be at most this long: the address holds 108 bytes, one of them the NUL.
inline constexpr std::string_view kDefaultSocketName = "fillscope-frames";
inline constexpr std::size_t kMaxSocketNameLength = 107;

// Owns a file descriptor and closes it on destruction: RAII, so a descriptor can't leak on an
// early return. Move-only, because two owners would close it twice.
class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) noexcept : fd_{fd} {}
    // std::exchange returns the old value and stores the new one: the moved-from Fd is left
    // empty (-1), so its destructor closes nothing.
    Fd(Fd&& other) noexcept : fd_{std::exchange(other.fd_, -1)} {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    ~Fd() { reset(); }

    [[nodiscard]] int get() const noexcept { return fd_; }
    // explicit: usable in `if (fd)`, but not silently converted to bool elsewhere.
    explicit operator bool() const noexcept { return fd_ >= 0; }
    void reset() noexcept;

private:
    int fd_ = -1;
};

// A non-blocking listening socket, or why there isn't one (EADDRINUSE: another listener has the
// name; ENAMETOOLONG: the name is too long).
[[nodiscard]] std::expected<Fd, std::error_code> listen_on(std::string_view name);

// Connects to a listener, making `attempts` tries `retry_delay` apart while it may still be
// starting. Never blocks for long: a listener whose queue is full counts as a failed attempt. On
// failure, the reason from the last attempt.
[[nodiscard]] std::expected<Fd, std::error_code> connect_to(std::string_view name, int attempts,
                                                             std::chrono::milliseconds retry_delay);

// Sends one message, or says why it couldn't. Blocks while the receiver's queue is full.
[[nodiscard]] std::expected<void, std::error_code> send_message(const Fd& socket, const Message& message);

// Whether a send failed because the receiver has gone (closed its end), rather than for some
// other reason.
[[nodiscard]] bool receiver_gone(std::error_code error) noexcept;

// What one call to MessageReceiver::drain did.
struct DrainResult {
    std::size_t received = 0;  // messages appended
    bool new_sender = false;   // a new sender was accepted; its messages start a new connection
};

// The viewer's side: accepts one sender at a time and drains its messages, never blocking, so it
// can be called once per paint. While one sender is connected, later senders wait in the
// listener's queue (one place) and are accepted when it leaves.
class MessageReceiver {
public:
    // At most this many messages are taken per call, so one call's time is bounded even if a
    // sender keeps the socket full; the rest wait for the next call.
    static constexpr std::size_t kMaxMessagesPerDrain = 10'000;

    explicit MessageReceiver(Fd listener) : listener_{std::move(listener)} {}

    // Accepts a sender if none is connected, then appends the messages waiting from it. A sender
    // that disconnects is dropped; the next is accepted on a later call. A sender that sends
    // something that isn't a message is dropped too, and error() says why.
    DrainResult drain(std::vector<Message>& out);

    [[nodiscard]] bool connected() const noexcept { return static_cast<bool>(sender_); }

    // Why the current or last sender was dropped for sending something invalid. Empty if it
    // wasn't, and cleared when a new sender is accepted.
    [[nodiscard]] const std::optional<std::string>& error() const noexcept { return error_; }

private:
    void drop_sender(std::string reason);

    Fd listener_;
    Fd sender_;
    std::optional<std::string> error_;
};

}  // namespace fillscope
