#include "channel/net.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <span>
#include <thread>

namespace fillscope {
namespace {

static_assert(kMaxSocketNameLength + 1 == sizeof(sockaddr_un::sun_path));

// The current errno as a std::error_code. Read it immediately after the failing call: any later
// call may change errno.
std::error_code last_error() noexcept { return {errno, std::system_category()}; }

struct Address {
    sockaddr_un address{};
    socklen_t length = 0;
};

// An abstract-namespace address: sun_path is '\0' followed by the name, and the length counts
// exactly those bytes (no terminator). A name too long is refused rather than cut short: two names
// that differed only past the limit would otherwise be the same address.
std::expected<Address, std::error_code> make_address(std::string_view name) {
    // An empty name is refused: bound, it would make the kernel choose a name (autobind), which no
    // other process could know.
    if (name.empty()) {
        return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
    }
    if (name.size() > kMaxSocketNameLength) {
        return std::unexpected{std::make_error_code(std::errc::filename_too_long)};
    }
    Address result;
    result.address.sun_family = AF_UNIX;
    std::memcpy(result.address.sun_path + 1, name.data(), name.size());
    result.length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size());
    return result;
}

// The socket layer's API takes the generic sockaddr*; a sockaddr_un is one of its forms.
const sockaddr* as_sockaddr(const sockaddr_un& address) noexcept { return reinterpret_cast<const sockaddr*>(&address); }

// Whether the peer of a connected socket has closed its end, without waiting.
bool peer_closed(const Fd& socket) noexcept {
    pollfd poll_request{.fd = socket.get(), .events = POLLRDHUP, .revents = 0};
    return ::poll(&poll_request, 1, 0) == 1 && (poll_request.revents & (POLLRDHUP | POLLHUP)) != 0;
}

// Whether a non-empty datagram is waiting on a connected socket, without taking it. MSG_PEEK
// leaves it queued; a one-byte buffer is enough, since only whether something arrived matters.
bool datagram_waiting(const Fd& socket) noexcept {
    std::byte byte{};
    return ::recv(socket.get(), &byte, 1, MSG_PEEK | MSG_DONTWAIT) > 0;
}

// Whether a failed accept4 just means nobody was waiting (or the one waiting gave up), rather than
// a fault worth reporting, such as having no file descriptors left.
bool nobody_to_accept(int error) noexcept {
    return error == EAGAIN || error == EWOULDBLOCK || error == EINTR || error == ECONNABORTED;
}

}  // namespace

void Fd::reset() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

// Every socket is created with SOCK_CLOEXEC, so it is closed in any program this process starts
// (with exec) rather than leaking into it.
std::expected<Fd, std::error_code> listen_on(std::string_view name) {
    const auto address = make_address(name);
    if (!address) {
        return std::unexpected{address.error()};
    }
    Fd socket{::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
    if (!socket) {
        return std::unexpected{last_error()};
    }
    if (::bind(socket.get(), as_sockaddr(address->address), address->length) != 0 || ::listen(socket.get(), 1) != 0) {
        return std::unexpected{last_error()};
    }
    return socket;
}

std::expected<Fd, std::error_code> connect_to(std::string_view name, int attempts,
                                              std::chrono::milliseconds retry_delay) {
    if (attempts < 1) {
        return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
    }
    const auto address = make_address(name);
    if (!address) {
        return std::unexpected{address.error()};
    }
    std::error_code error;  // set by every failed attempt, and there is at least one
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (attempt > 0) {
            std::this_thread::sleep_for(retry_delay);  // between attempts, not after the last
        }
        // Non-blocking while connecting, so a listener whose queue is full fails at once (EAGAIN)
        // instead of blocking until it has room; blocking again afterwards, so sends wait for the
        // receiver rather than fail when its queue is momentarily full.
        Fd socket{::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
        if (!socket) {
            return std::unexpected{last_error()};
        }
        if (::connect(socket.get(), as_sockaddr(address->address), address->length) != 0) {
            error = last_error();
            continue;
        }
        const int flags = ::fcntl(socket.get(), F_GETFL);
        if (flags < 0 || ::fcntl(socket.get(), F_SETFL, flags & ~O_NONBLOCK) != 0) {
            return std::unexpected{last_error()};
        }
        return socket;
    }
    return std::unexpected{error};
}

std::expected<void, std::error_code> wait_until_accepted(const Fd& socket, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        pollfd poll_request{.fd = socket.get(), .events = POLLIN, .revents = 0};
        const int ready = ::poll(&poll_request, 1, static_cast<int>(std::max(left.count(), std::int64_t{0})));
        if (ready < 0 && errno == EINTR) {
            continue;  // a signal arrived; wait for the rest of the time
        }
        if (ready < 0) {
            return std::unexpected{last_error()};
        }
        if (ready == 0) {
            return std::unexpected{std::make_error_code(std::errc::timed_out)};
        }
        std::byte acknowledgement{};
        const ssize_t n = ::recv(socket.get(), &acknowledgement, 1, 0);
        if (n == 1) {
            return {};
        }
        if (n == 0) {
            return std::unexpected{std::make_error_code(std::errc::connection_refused)};  // closed: refused
        }
        if (errno != EINTR) {
            return std::unexpected{last_error()};
        }
    }
}

std::expected<void, std::error_code> send_message(const Fd& socket, const Message& message) {
    const EncodedMessage encoded = encode(message);
    while (true) {
        // MSG_NOSIGNAL: a gone receiver is reported as EPIPE here rather than by a SIGPIPE that
        // would end the process.
        const ssize_t sent = ::send(socket.get(), encoded.bytes.data(), encoded.size, MSG_NOSIGNAL);
        if (sent >= 0) {
            // A SEQPACKET send is all or nothing: one datagram, whole.
            return {};
        }
        if (errno != EINTR) {  // EINTR: a signal interrupted the call before anything was sent; try again
            return std::unexpected{last_error()};
        }
    }
}

bool receiver_gone(std::error_code error) noexcept {
    return error == std::errc::broken_pipe || error == std::errc::connection_reset;
}

void MessageReceiver::drop_sender(std::string reason) {
    error_ = std::move(reason);
    sender_.reset();  // a sender speaking something else can't be trusted further
}

std::optional<Fd> MessageReceiver::accept_waiting() {
    Fd accepted{::accept4(listener_.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC)};
    if (accepted) {
        return accepted;
    }
    if (!nobody_to_accept(errno)) {
        error_ = std::format("can't accept a source: {}", last_error().message());
    }
    return std::nullopt;
}

DrainResult MessageReceiver::drain(std::vector<Message>& out) {
    DrainResult result;
    if (!sender_) {
        if (auto accepted = accept_waiting()) {
            // Tells the sender it was accepted (wait_until_accepted). It can only fail if the
            // sender has already gone, which the first recv below then finds.
            constexpr std::byte kAccepted{1};
            (void)::send(accepted->get(), &kAccepted, 1, MSG_NOSIGNAL | MSG_DONTWAIT);
            sender_ = std::move(*accepted);
            error_.reset();
            result.new_sender = true;
        }
    }
    // One byte more than the largest message, so an oversized datagram arrives as oversized (and
    // fails to decode) rather than cut down to a size that might decode.
    std::array<std::byte, kMaxMessageSize + 1> buffer;
    while (sender_ && result.received < max_per_drain_) {
        const ssize_t n = ::recv(sender_.get(), buffer.data(), buffer.size(), 0);
        if (n > 0) {
            auto message = decode(std::span{buffer}.first(static_cast<std::size_t>(n)));
            if (!message) {
                drop_sender(std::move(message.error()));
                break;
            }
            out.push_back(*message);
            ++result.received;
        } else if (n == 0) {
            // recv returns 0 both when the sender has closed its end and for an empty datagram,
            // which no sender of ours sends. A sender still connected sent an empty datagram. One
            // that has closed may have sent one before closing, with more queued after it: then a
            // datagram is still waiting, and the 0 was an empty message, not the end. (An empty
            // datagram that is the very last thing sent before closing can't be told from the end.)
            if (!peer_closed(sender_) || datagram_waiting(sender_)) {
                drop_sender("an empty message");
            } else {
                sender_.reset();
            }
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;  // nothing more waiting
        } else if (errno == EINTR) {
            continue;  // interrupted by a signal before anything arrived; try again
        } else if (errno == ECONNRESET) {
            sender_.reset();  // the sender went away abruptly: a disconnect, not a fault
        } else {
            drop_sender(std::format("the connection failed: {}", last_error().message()));
        }
    }
    // One source at a time: any other that connected meanwhile is refused (accepted and closed at
    // once), so its sends fail and it says so, rather than seeming to succeed into a queue that
    // would be read only after this source leaves.
    while (sender_) {
        if (!accept_waiting()) {
            break;
        }
    }
    return result;
}

}  // namespace fillscope
