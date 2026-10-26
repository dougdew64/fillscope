#include "viewer/crash_log.h"

#include <execinfo.h>
#include <signal.h>
#include <unistd.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <format>
#include <string_view>

#if defined(__SANITIZE_ADDRESS__)
#include <sanitizer/common_interface_defs.h>
#endif

namespace fillscope {
namespace {

// Set once, before any handler can run; read by the handlers.
int g_log_fd = -1;

struct FatalSignal {
    int number;
    const char* name;
};
constexpr std::array kFatalSignals{FatalSignal{SIGSEGV, "SIGSEGV"}, FatalSignal{SIGBUS, "SIGBUS"},
                                   FatalSignal{SIGFPE, "SIGFPE"}, FatalSignal{SIGILL, "SIGILL"},
                                   FatalSignal{SIGABRT, "SIGABRT"}};

// What a signal handler may call: write(2) (async-signal-safe), no allocation, no locks, no stdio.
void write_all(const char* text, std::size_t size) {
    while (size > 0) {
        const ssize_t written = ::write(g_log_fd, text, size);
        if (written <= 0) {
            return;
        }
        text += written;
        size -= static_cast<std::size_t>(written);
    }
}

void write_text(const char* text) { write_all(text, std::strlen(text)); }

// The decimal digits of `n`, written without allocating.
void write_number(int n) {
    std::array<char, 16> digits{};
    std::size_t at = digits.size();
    unsigned value = n < 0 ? 0u - static_cast<unsigned>(n) : static_cast<unsigned>(n);
    do {
        digits[--at] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0 && at > 1);
    if (n < 0) {
        digits[--at] = '-';
    }
    write_all(digits.data() + at, digits.size() - at);
}

const char* fatal_name(int signal) {
    for (const FatalSignal& fatal : kFatalSignals) {
        if (fatal.number == signal) {
            return fatal.name;
        }
    }
    return nullptr;
}

extern "C" void record_fatal_signal(int signal) {
    // No timestamp: the time functions that format one aren't safe here. The log's last line before
    // this one says when.
    write_text("crash: fatal signal ");
    write_number(signal);
    if (const char* name = fatal_name(signal)) {
        write_text(" (");
        write_text(name);
        write_text(")");
    }
    write_text("; the backtrace, innermost first:\n");
    std::array<void*, 64> frames{};
    const int count = ::backtrace(frames.data(), static_cast<int>(frames.size()));
    ::backtrace_symbols_fd(frames.data(), count, g_log_fd);
    write_text("crash: end of the backtrace; the signal now ends the process\n");
    // SA_RESETHAND has restored the default action: raised again, the signal ends the process as it
    // would have. (Returning would do it too for a fault, which happens again; not for a signal sent
    // with kill.)
    ::raise(signal);
}

[[noreturn]] void record_terminate() {
    std::string line = "crash: std::terminate";
    if (const std::exception_ptr current = std::current_exception()) {
        try {
            std::rethrow_exception(current);
        } catch (const std::exception& error) {
            line += std::format(", an uncaught exception: {}", error.what());
        } catch (...) {
            line += ", an uncaught exception that isn't a std::exception";
        }
    }
    line += "; it aborts\n";
    write_all(line.data(), line.size());
    std::abort();  // recorded with a backtrace by the SIGABRT handler
}

// The handlers' own stack, for a crash that is a stack overflow: the thread's stack is then used up.
alignas(16) std::array<char, 64 * 1024> g_alternate_stack{};

}  // namespace

std::string signal_name(int signal) {
    const char* name = fatal_name(signal);
    return name ? std::string{name} : std::format("signal {}", signal);
}

std::string install_crash_log(int log_fd, const std::filesystem::path& sanitizer_report) {
    g_log_fd = log_fd;
    // backtrace() loads libgcc the first time it is called, which may allocate: done here, so the
    // handler's call doesn't.
    std::array<void*, 4> warm{};
    (void)::backtrace(warm.data(), static_cast<int>(warm.size()));

    stack_t stack{};
    stack.ss_sp = g_alternate_stack.data();
    stack.ss_size = g_alternate_stack.size();
    (void)::sigaltstack(&stack, nullptr);

    std::string handled;
    std::string left;
    for (const FatalSignal& fatal : kFatalSignals) {
#if defined(__SANITIZE_ADDRESS__)
        if (fatal.number == SIGSEGV || fatal.number == SIGBUS || fatal.number == SIGFPE) {
            left += left.empty() ? fatal.name : std::format(", {}", fatal.name);
            continue;
        }
#endif
        struct sigaction action{};
        action.sa_handler = record_fatal_signal;
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_RESETHAND | SA_ONSTACK;
        if (::sigaction(fatal.number, &action, nullptr) == 0) {
            handled += handled.empty() ? fatal.name : std::format(", {}", fatal.name);
        }
    }
    std::set_terminate(record_terminate);

    std::string said = std::format("crash recording: {} and std::terminate recorded here", handled);
#if defined(__SANITIZE_ADDRESS__)
    __sanitizer_set_report_path(sanitizer_report.c_str());
    said += std::format("; {} left to AddressSanitizer, whose reports, and UndefinedBehaviorSanitizer's, go to {}.<pid>", left,
                        sanitizer_report.string());
#else
    (void)sanitizer_report;
#endif
    return said;
}

}  // namespace fillscope
