#include "feed/feed_socket.h"

#include <arpa/inet.h>
#include <cerrno>
#include <format>
#include <netinet/in.h>
#include <sys/socket.h>

namespace fillscope {

namespace {

std::error_code last_socket_error() noexcept { return {errno, std::system_category()}; }

}  // namespace

std::expected<FeedSocket, std::error_code> FeedSocket::join(const Stream& stream) {
    in_addr group{};
    if (inet_pton(AF_INET, stream.group.c_str(), &group) != 1) {
        return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
    }
    Fd fd{::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_UDP)};
    if (!fd) {
        return std::unexpected{last_socket_error()};
    }
    const int one = 1;
    // Others listen on the same port: the trading clients, and the exchange's other readers.
    if (::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) != 0) {
        return std::unexpected{last_socket_error()};
    }
    // Bound to the group's address, not to any address: only this group's datagrams arrive, not those
    // of every group joined on this machine with the same port.
    const sockaddr_in address{.sin_family = AF_INET, .sin_port = htons(stream.port), .sin_addr = group, .sin_zero = {}};
    if (::bind(fd.get(), reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0) {
        return std::unexpected{last_socket_error()};
    }
    // On the loopback interface, which the exchange sends on (feed_socket.h).
    const ip_mreq membership{.imr_multiaddr = group, .imr_interface = {htonl(INADDR_LOOPBACK)}};
    if (::setsockopt(fd.get(), IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof membership) != 0) {
        return std::unexpected{last_socket_error()};
    }
    return FeedSocket{std::move(fd)};
}

FeedSocket::Received FeedSocket::receive(std::vector<MarketUpdate>& out, std::size_t max_datagrams) {
    Received received;
    while (received.datagrams < max_datagrams) {
        const ssize_t n = ::recv(fd_.get(), buffer_.data(), buffer_.size(), 0);
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                received.error = std::format("receiving market data failed: {}", last_socket_error().message());
            }
            break;
        }
        ++received.datagrams;
        auto updates = decode_updates(std::span<const std::byte>{buffer_.data(), static_cast<std::size_t>(n)});
        if (!updates) {
            received.error = std::move(updates.error());
            continue;
        }
        out.insert(out.end(), updates->begin(), updates->end());
    }
    return received;
}

}  // namespace fillscope
