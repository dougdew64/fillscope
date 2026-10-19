#include "viewer/stop_signals.h"

#include <cerrno>
#include <csignal>

namespace fillscope {
namespace {

// A signal handler may only touch a `volatile std::sig_atomic_t` (or a lock-free atomic): it can
// run between any two instructions of the loop that reads the flag.
volatile std::sig_atomic_t g_stop_requested = 0;

extern "C" void request_stop(int /*signal*/) { g_stop_requested = 1; }

}  // namespace

std::expected<void, std::error_code> install_stop_signals() {
    struct sigaction action{};
    action.sa_handler = request_stop;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESETHAND;  // the second signal gets the default action: the process ends
    for (const int signal : {SIGINT, SIGTERM}) {
        if (::sigaction(signal, &action, nullptr) != 0) {
            return std::unexpected{std::error_code{errno, std::system_category()}};
        }
    }
    return {};
}

bool stop_requested() noexcept { return g_stop_requested != 0; }

}  // namespace fillscope
