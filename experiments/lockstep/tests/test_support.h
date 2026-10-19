// Helpers shared by the tests: unique socket names, and child processes.
#pragma once

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

extern char** environ;

namespace lockstep::test {

// A socket name no other test, and no running viewer, is using.
inline std::string unique_socket_name() {
    static std::atomic<int> counter{0};
    return "fillscope-test-" + std::to_string(::getpid()) + "-" + std::to_string(counter++);
}

// A child process started with posix_spawn. The destructor waits for it, so none is left behind.
class ChildProcess {
public:
    explicit ChildProcess(const std::vector<std::string>& argv) {
        std::vector<char*> args;
        for (const auto& arg : argv) {
            args.push_back(const_cast<char*>(arg.c_str()));
        }
        args.push_back(nullptr);
        if (::posix_spawn(&pid_, args[0], nullptr, nullptr, args.data(), environ) != 0) {
            pid_ = -1;
        }
    }
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ~ChildProcess() { (void)wait(); }

    [[nodiscard]] bool started() const { return pid_ > 0; }

    // The exit status, or -1 if it didn't exit normally. Waits the first time; later calls return
    // the same answer.
    int wait() {
        if (pid_ > 0) {
            int status = 0;
            ::waitpid(pid_, &status, 0);
            exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            pid_ = -1;
        }
        return exit_code_;
    }

private:
    pid_t pid_ = -1;
    int exit_code_ = -1;
};

// Runs a shell command and returns everything it wrote to stdout and stderr.
inline std::string run_command(const std::string& command) {
    std::string output;
    if (FILE* pipe = ::popen((command + " 2>&1").c_str(), "r")) {
        char buffer[4096];
        while (std::size_t n = std::fread(buffer, 1, sizeof buffer, pipe)) {
            output.append(buffer, n);
        }
        ::pclose(pipe);
    }
    return output;
}

inline bool command_exists(const std::string& name) {
    return !run_command("command -v " + name).empty();
}

}  // namespace lockstep::test
