// Helpers shared by the tests: shell commands, and sending to a multicast group.
#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace fillscope::test {

// `text` quoted for a POSIX shell: in single quotes, with each single quote inside written as '\''.
inline std::string shell_quote(std::string_view text) {
    std::string quoted = "'";
    for (const char c : text) {
        quoted += c == '\'' ? std::string{"'\\''"} : std::string{c};
    }
    return quoted + "'";
}

struct CommandResult {
    std::string output;  // everything written to stdout and stderr
    int status = -1;     // the exit status, or -1 if the command couldn't be run or didn't exit
};

// Runs a shell command, collecting its output and exit status.
inline CommandResult run_command(const std::string& command) {
    CommandResult result;
    FILE* pipe = ::popen((command + " 2>&1").c_str(), "r");
    if (pipe == nullptr) {
        return result;
    }
    std::array<char, 4096> buffer;
    while (const std::size_t n = std::fread(buffer.data(), 1, buffer.size(), pipe)) {
        result.output.append(buffer.data(), n);
    }
    const int status = ::pclose(pipe);
    result.status = status != -1 && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}

inline bool command_exists(const std::string& name) {
    return run_command("command -v " + shell_quote(name)).status == 0;
}

// Sends one datagram to a multicast group as the exchange does: on the loopback interface. Returns
// whether all of it was sent.
inline bool send_multicast(const std::string& group, std::uint16_t port, const void* data, std::size_t size) {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        return false;
    }
    const in_addr loopback{htonl(INADDR_LOOPBACK)};
    sockaddr_in address{.sin_family = AF_INET, .sin_port = htons(port), .sin_addr = {}, .sin_zero = {}};
    const bool ok = ::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &loopback, sizeof loopback) == 0 &&
                    ::inet_pton(AF_INET, group.c_str(), &address.sin_addr) == 1 &&
                    ::sendto(fd, data, size, 0, reinterpret_cast<const sockaddr*>(&address), sizeof address) ==
                        static_cast<ssize_t>(size);
    ::close(fd);
    return ok;
}

}  // namespace fillscope::test
