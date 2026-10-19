#include "channel/net.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <span>
#include <thread>

namespace fillscope {
namespace {

// Fills an abstract-namespace address: sun_path is '\0' followed by the name, and the address
// length counts exactly those bytes (no terminator).
socklen_t make_address(std::string_view name, sockaddr_un& addr) {
    addr = {};
    addr.sun_family = AF_UNIX;
    const std::size_t len = std::min(name.size(), sizeof addr.sun_path - 1);
    std::memcpy(addr.sun_path + 1, name.data(), len);
    return static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + len);
}

}  // namespace

void Fd::reset() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

Fd listen_on(std::string_view name) {
    Fd fd{::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0)};
    sockaddr_un addr;
    const socklen_t len = make_address(name, addr);
    if (!fd || ::bind(fd.get(), reinterpret_cast<sockaddr*>(&addr), len) != 0 ||
        ::listen(fd.get(), 1) != 0) {
        return Fd{};
    }
    return fd;
}

Fd connect_to(std::string_view name, int attempts, std::chrono::milliseconds retry_delay) {
    sockaddr_un addr;
    const socklen_t len = make_address(name, addr);
    for (int attempt = 0; attempt < attempts; ++attempt) {
        Fd fd{::socket(AF_UNIX, SOCK_SEQPACKET, 0)};
        if (fd && ::connect(fd.get(), reinterpret_cast<sockaddr*>(&addr), len) == 0) {
            return fd;
        }
        std::this_thread::sleep_for(retry_delay);
    }
    return Fd{};
}

bool send_message(int fd, const Message& message) {
    const EncodedMessage encoded = encode(message);
    // MSG_NOSIGNAL: a gone viewer is reported as an error here, not as a SIGPIPE.
    return ::send(fd, encoded.bytes.data(), encoded.size, MSG_NOSIGNAL) == static_cast<ssize_t>(encoded.size);
}

std::size_t MessageReceiver::drain(std::vector<Message>& out) {
    if (!sender_) {
        sender_ = Fd{::accept4(listener_.get(), nullptr, nullptr, SOCK_NONBLOCK)};
    }
    std::size_t received = 0;
    // One byte more than the largest message, so an oversized datagram is seen as oversized
    // rather than silently cut to a size that might decode.
    std::array<std::byte, kMaxMessageSize + 1> buffer;
    while (sender_) {
        const ssize_t n = ::recv(sender_.get(), buffer.data(), buffer.size(), 0);
        if (n > 0) {
            auto message = decode(std::span{buffer}.first(static_cast<std::size_t>(n)));
            if (!message) {
                error_ = std::move(message.error());
                sender_.reset();  // a sender speaking something else can't be trusted further
                break;
            }
            out.push_back(*message);
            ++received;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;  // nothing more waiting
        } else {
            sender_.reset();  // 0: the sender closed its end; anything else: a broken connection
        }
    }
    return received;
}

}  // namespace fillscope
