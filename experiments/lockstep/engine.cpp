// The engine: produces frames, sends each to the viewer, then calls the anchor.
//
// Under a debugger, the anchor is where execution stops after every frame, so each Continue
// advances the viewer's animation by exactly one frame. The viewer is a separate process, so it
// keeps painting while this one is stopped.
//
//   engine [--frames N] [--interval-ms MS] [--self-stop]
//
// --self-stop: the anchor raises SIGTRAP when a debugger is attached, so no breakpoint has to be
// armed. Without a debugger it does nothing (SIGTRAP's default action would kill the process).

#include "frame.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <thread>

namespace {

// Plain, not std::atomic: only this thread touches it. An atomic's store() is inlined from a
// libstdc++ header even at -O0, so a breakpoint on the anchor by name landed in atomic_base.h.
std::uint64_t g_last_anchor_index = 0;
bool g_self_stop = false;

// Whether a debugger is attached: Linux reports the tracer's pid in /proc/self/status.
bool debugger_attached() {
    std::ifstream status{"/proc/self/status"};
    for (std::string line; std::getline(status, line);) {
        if (line.starts_with("TracerPid:")) {
            return std::stoi(line.substr(10)) != 0;
        }
    }
    return false;
}

int connect_to_viewer() {
    for (int attempt = 0; attempt < 50; ++attempt) {  // the viewer may still be starting
        int fd = ::socket(AF_UNIX, SOCK_SEQPACKET, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::memcpy(addr.sun_path, lockstep::kSocketName.data(), lockstep::kSocketName.size());
        auto len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + lockstep::kSocketName.size());
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), len) == 0) {
            return fd;
        }
        ::close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return -1;
}

}  // namespace

// The breakpoint anchor. It must keep a body after optimization: an empty function can be merged
// with every other empty function (GCC's -fipa-icf, on at -O2), and a breakpoint on it would then
// fire from unrelated code. The store has a reader (main prints it), and the asm statement tells
// the compiler memory may be read here, so the store cannot be removed.
extern "C" [[gnu::noinline]] void fillscope_frame_anchor(std::uint64_t frame_index) {
    g_last_anchor_index = frame_index;
    asm volatile("" ::: "memory");
    if (g_self_stop && debugger_attached()) {
        // A breakpoint instruction, executed here; raise(SIGTRAP) stopped two frames down inside
        // libc under GDB.
        asm volatile("int3");  // ---- the debugger stops here; Continue for the next frame ----
    }
}

int main(int argc, char** argv) {
    std::uint64_t frame_count = 1000;
    int interval_ms = 200;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};
        if (arg == "--frames" && i + 1 < argc) {
            frame_count = std::stoull(argv[++i]);
        } else if (arg == "--interval-ms" && i + 1 < argc) {
            interval_ms = std::stoi(argv[++i]);
        } else if (arg == "--self-stop") {
            g_self_stop = true;
        } else {
            std::fprintf(stderr, "usage: engine [--frames N] [--interval-ms MS] [--self-stop]\n");
            return 2;
        }
    }

    int fd = connect_to_viewer();
    if (fd < 0) {
        std::fprintf(stderr, "engine: no viewer listening\n");
        return 1;
    }

    std::mt19937_64 rng{42};  // fixed seed: the same run every time
    std::uniform_int_distribution<int> step{-2, 2};
    std::uniform_int_distribution<std::uint32_t> qty{1, 500};
    std::int64_t price = 10'000;

    for (std::uint64_t index = 0; index < frame_count; ++index) {
        price += step(rng);
        lockstep::Frame frame{index, price, qty(rng), 0};
        if (::send(fd, &frame, sizeof frame, MSG_NOSIGNAL) != static_cast<ssize_t>(sizeof frame)) {
            std::fprintf(stderr, "engine: viewer went away at frame %llu\n",
                         static_cast<unsigned long long>(index));
            break;
        }
        fillscope_frame_anchor(index);
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    }
    ::close(fd);
    std::printf("engine: last anchored frame %llu\n",
                static_cast<unsigned long long>(g_last_anchor_index));
    return 0;
}
