// Helpers shared by the tests: unique socket names, a listening receiver, child processes, shell
// commands, and sending to a multicast group.
#pragma once

#include "channel/net.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <mutex>
#include <stop_token>
#include <thread>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

extern char** environ;

namespace fillscope::test {

// A socket name no other test, and no running viewer, is using.
inline std::string unique_socket_name() {
    static std::atomic<int> counter{0};
    return "fillscope-test-" + std::to_string(::getpid()) + "-" + std::to_string(counter++);
}

// A receiver listening on `name`. Throws if it can't listen, which GoogleTest reports as the
// test's failure, with the reason.
inline MessageReceiver listen(std::string_view name) {
    auto listener = listen_on(name);
    if (!listener) {
        throw std::runtime_error("can't listen on " + std::string{name} + ": " + listener.error().message());
    }
    return MessageReceiver{std::move(*listener)};
}

// A receiver drained on a thread of its own, every millisecond, for tests that run a frame source
// and look at what arrived afterwards: a frame source waits until a drain accepts its connection,
// so draining only afterwards would leave it waiting.
class BackgroundReceiver {
public:
    explicit BackgroundReceiver(std::string_view name) : receiver_{listen(name)} {}
    BackgroundReceiver(const BackgroundReceiver&) = delete;
    BackgroundReceiver& operator=(const BackgroundReceiver&) = delete;

    // Everything received so far.
    [[nodiscard]] std::vector<Message> messages() const {
        const std::scoped_lock lock{mutex_};
        return messages_;
    }

private:
    void run(const std::stop_token& stop) {
        while (!stop.stop_requested()) {
            {
                const std::scoped_lock lock{mutex_};
                receiver_.drain(messages_);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }

    MessageReceiver receiver_;
    mutable std::mutex mutex_;
    std::vector<Message> messages_;
    // Declared last, so it starts after everything it uses exists, and (as std::jthread asks its
    // thread to stop and joins it in its destructor) stops before any of it is destroyed.
    std::jthread thread_{[this](const std::stop_token& stop) { run(stop); }};
};

// A child process started with posix_spawn. The destructor waits for it, so none is left behind.
class ChildProcess {
public:
    explicit ChildProcess(const std::vector<std::string>& argv) {
        std::vector<char*> args;
        for (const auto& arg : argv) {
            args.push_back(const_cast<char*>(arg.c_str()));  // posix_spawn's C interface takes char*
        }
        args.push_back(nullptr);
        started_ = ::posix_spawn(&pid_, args[0], nullptr, nullptr, args.data(), environ) == 0;
        if (!started_) {
            pid_ = -1;
        }
    }
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ~ChildProcess() { (void)wait(); }

    // Whether the process was started, whether or not it has been waited for since.
    [[nodiscard]] bool started() const { return started_; }

    // The exit status, or -1 if the process didn't exit normally or couldn't be waited for. Waits
    // the first time; later calls return the same answer.
    int wait() {
        if (pid_ > 0) {
            int status = 0;
            pid_t waited = 0;
            do {
                waited = ::waitpid(pid_, &status, 0);
            } while (waited < 0 && errno == EINTR);  // interrupted by a signal: wait again
            exit_code_ = waited == pid_ && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            pid_ = -1;
        }
        return exit_code_;
    }

private:
    bool started_ = false;
    pid_t pid_ = -1;
    int exit_code_ = -1;
};

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

// Sends one datagram to a multicast group as the exchange does: on the interface the system routes
// the group to, with no interface chosen (its sockets name "lo" but don't use it), looped back to this
// machine's listeners. Returns whether all of it was sent.
inline bool send_multicast(const std::string& group, std::uint16_t port, const void* data, std::size_t size) {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        return false;
    }
    sockaddr_in address{.sin_family = AF_INET, .sin_port = htons(port), .sin_addr = {}, .sin_zero = {}};
    const bool ok = ::inet_pton(AF_INET, group.c_str(), &address.sin_addr) == 1 &&
                    ::sendto(fd, data, size, 0, reinterpret_cast<const sockaddr*>(&address), sizeof address) ==
                        static_cast<ssize_t>(size);
    ::close(fd);
    return ok;
}

}  // namespace fillscope::test
