// The socket that carries frames to the viewer: a Unix SOCK_SEQPACKET socket in Linux's abstract
// namespace, so each send is one whole frame and there is no socket file to clean up.
#pragma once

#include "channel/frame.h"

#include <chrono>
#include <cstddef>
#include <string_view>
#include <utility>
#include <vector>

namespace fillscope {

// Socket names are given without the abstract namespace's leading NUL; the functions add it.
inline constexpr std::string_view kDefaultSocketName = "fillscope-frames";

// Owns a file descriptor and closes it on destruction. Move-only.
class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) noexcept : fd_{fd} {}
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
    explicit operator bool() const noexcept { return fd_ >= 0; }
    void reset() noexcept;

private:
    int fd_ = -1;
};

// A non-blocking listening socket, or an empty Fd if the name is taken.
[[nodiscard]] Fd listen_on(std::string_view name);

// Connects to a listener, retrying while it may still be starting. An empty Fd if none appeared.
[[nodiscard]] Fd connect_to(std::string_view name, int attempts, std::chrono::milliseconds retry_delay);

// Sends one frame; false if the other side has gone.
[[nodiscard]] bool send_frame(int fd, const Frame& frame);

// The viewer's side: accepts one sender at a time and drains its frames, never blocking.
class FrameReceiver {
public:
    explicit FrameReceiver(Fd listener) : listener_{std::move(listener)} {}

    // Accepts a sender if none is connected, then appends every frame waiting on the socket. A
    // sender that disconnects is dropped, and the next one is accepted. Returns how many frames
    // were appended.
    std::size_t drain(std::vector<Frame>& out);

    [[nodiscard]] bool connected() const noexcept { return static_cast<bool>(sender_); }

private:
    Fd listener_;
    Fd sender_;
};

}  // namespace fillscope
