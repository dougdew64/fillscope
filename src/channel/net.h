// The socket that carries messages (channel/message.h) from a frame source to the viewer: a Unix
// SOCK_SEQPACKET socket in Linux's abstract namespace. SEQPACKET keeps message boundaries (each
// send arrives as one whole datagram) and delivers in order; the abstract namespace means there is
// no socket file on disk to clean up.
#pragma once

#include "channel/message.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fillscope {

// Socket names are given without the abstract namespace's leading NUL; the functions add it.
inline constexpr std::string_view kDefaultSocketName = "fillscope-frames";

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

// A non-blocking listening socket, or an empty Fd if the name is taken.
[[nodiscard]] Fd listen_on(std::string_view name);

// Connects to a listener, retrying while it may still be starting. An empty Fd if none appeared.
[[nodiscard]] Fd connect_to(std::string_view name, int attempts, std::chrono::milliseconds retry_delay);

// Sends one message; false if the other side has gone.
[[nodiscard]] bool send_message(int fd, const Message& message);

// The viewer's side: accepts one sender at a time and drains its messages, never blocking, so it
// can be called once per paint.
class MessageReceiver {
public:
    explicit MessageReceiver(Fd listener) : listener_{std::move(listener)} {}

    // Accepts a sender if none is connected, then appends every message waiting on the socket.
    // A sender that disconnects is dropped and the next one accepted. A datagram that isn't a valid
    // message also drops the sender, and error() says why. Returns how many messages were appended.
    std::size_t drain(std::vector<Message>& out);

    [[nodiscard]] bool connected() const noexcept { return static_cast<bool>(sender_); }

    // Why the last sender was dropped for sending something invalid; empty if none was.
    [[nodiscard]] const std::optional<std::string>& error() const noexcept { return error_; }

private:
    Fd listener_;
    Fd sender_;
    std::optional<std::string> error_;
};

}  // namespace fillscope
