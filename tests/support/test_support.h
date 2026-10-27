// Helpers shared by the tests: shell commands, ports of a test's own, sending to a multicast group,
// running a back-end program, and a directory of a test's own.
#pragma once

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

extern char** environ;

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

// A port of this process's own, `index` from 0 to 9, for a test's multicast or TCP, by the rule in
// RUNNING.md (*Ports of their own*): the process claims a numbered slot, 0 to 999, by locking a file
// of that number in fillscope-test-ports in the temporary directory, and holds it until it exits;
// slot N has ports 22000 + 10 N to 22000 + 10 N + 9.
inline std::uint16_t test_port(int index) {
    static const int slot = [] {
        const auto directory = std::filesystem::temp_directory_path() / "fillscope-test-ports";
        std::error_code ignored;
        std::filesystem::create_directories(directory, ignored);
        for (int candidate = 0; candidate < 1000; ++candidate) {
            const auto path = directory / ("slot-" + std::to_string(candidate));
            const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0666);
            if (fd >= 0 && ::flock(fd, LOCK_EX | LOCK_NB) == 0) {
                return candidate;  // the descriptor stays open, and the lock held, until the process exits
            }
            if (fd >= 0) {
                ::close(fd);
            }
        }
        std::fprintf(stderr, "test_port: every slot in %s is taken\n", directory.c_str());
        std::abort();
    }();
    return static_cast<std::uint16_t>(22000 + 10 * slot + index);
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

// Whether something listens on TCP `port` on the loopback interface.
inline bool tcp_port_open(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    const sockaddr_in address{.sin_family = AF_INET, .sin_port = htons(port), .sin_addr = {htonl(INADDR_LOOPBACK)}, .sin_zero = {}};
    const bool open = ::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0;
    ::close(fd);
    return open;
}

// A back-end program, started in `directory` (it writes its logs there) and killed at the end: how
// it shuts down isn't the subject of the tests, and takes a minute.
class Program {
public:
    Program(const std::vector<std::string>& argv, const std::filesystem::path& directory) {
        std::vector<char*> args;
        for (const auto& arg : argv) {
            args.push_back(const_cast<char*>(arg.c_str()));
        }
        args.push_back(nullptr);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addchdir_np(&actions, directory.c_str());
        // Its output would bury the test's; its logs are in `directory`.
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        started_ = posix_spawn(&pid_, args[0], &actions, nullptr, args.data(), environ) == 0;
        posix_spawn_file_actions_destroy(&actions);
    }
    Program(const Program&) = delete;
    Program& operator=(const Program&) = delete;
    ~Program() {
        if (started_) {
            ::kill(pid_, SIGKILL);
            ::waitpid(pid_, nullptr, 0);
        }
    }
    [[nodiscard]] bool started() const { return started_; }
    [[nodiscard]] bool running() const { return started_ && ::waitpid(pid_, nullptr, WNOHANG) == 0; }

private:
    pid_t pid_ = -1;
    bool started_ = false;
};

// Runs a program in `directory` to its end, its output discarded; its exit status, or -1.
inline int run_to_exit(const std::vector<std::string>& argv, const std::filesystem::path& directory) {
    std::vector<char*> args;
    for (const auto& arg : argv) {
        args.push_back(const_cast<char*>(arg.c_str()));
    }
    args.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addchdir_np(&actions, directory.c_str());
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    pid_t pid = -1;
    const bool started = posix_spawn(&pid, args[0], &actions, nullptr, args.data(), environ) == 0;
    posix_spawn_file_actions_destroy(&actions);
    int status = 0;
    return started && ::waitpid(pid, &status, 0) == pid && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// A directory of the test's own in the temporary directory, `prefix-PID`, removed however the test
// ends (but a test killed outright leaves it).
struct TemporaryDirectory {
    explicit TemporaryDirectory(std::string_view prefix)
        : path{std::filesystem::temp_directory_path() / (std::string{prefix} + "-" + std::to_string(::getpid()))} {
        std::filesystem::create_directories(path);
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    const std::filesystem::path path;
};

}  // namespace fillscope::test
