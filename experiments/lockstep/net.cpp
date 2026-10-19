#include "net.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <thread>

namespace lockstep {
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

bool send_frame(int fd, const Frame& frame) {
    // MSG_NOSIGNAL: a gone viewer is reported as an error here, not as a SIGPIPE.
    return ::send(fd, &frame, sizeof frame, MSG_NOSIGNAL) == static_cast<ssize_t>(sizeof frame);
}

std::size_t FrameReceiver::drain(std::vector<Frame>& out) {
    if (!engine_) {
        engine_ = Fd{::accept4(listener_.get(), nullptr, nullptr, SOCK_NONBLOCK)};
    }
    std::size_t received = 0;
    while (engine_) {
        Frame frame;
        const ssize_t n = ::recv(engine_.get(), &frame, sizeof frame, 0);
        if (n == static_cast<ssize_t>(sizeof frame)) {
            out.push_back(frame);
            ++received;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;  // nothing more waiting
        } else {
            engine_.reset();  // 0: the engine closed its end; anything else: a broken connection
        }
    }
    return received;
}

}  // namespace lockstep
