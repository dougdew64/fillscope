// Records a crash of the viewer in its log (viewer_log.h): the fatal signal and a backtrace, and an
// uncaught exception's message. A crash ends the process before anything orderly can run, so what
// is recorded is written from the signal handler itself, straight to the log's descriptor, using
// only what a signal handler may call.
#pragma once

#include <filesystem>
#include <string>

namespace fillscope {

// Installs the handlers, writing to `log_fd` (which must stay open until the process ends):
//   - for the fatal signals, a line naming the signal and a backtrace, after which the signal ends
//     the process as it would have (the default action, so a core dump and the exit status are
//     unchanged); on an alternate stack, so a stack overflow is recorded too;
//   - std::terminate (an uncaught exception, among others): a line with the exception's message,
//     then abort, which the SIGABRT handler records with a backtrace.
// In a build with AddressSanitizer, whose own handlers report SIGSEGV, SIGBUS and SIGFPE (where
// and why, better than a backtrace can), those are left to it, and the sanitizers' reports are
// written to `sanitizer_report` with ".<pid>" appended, beside the log. Returns a line for the log
// saying what was installed.
std::string install_crash_log(int log_fd, const std::filesystem::path& sanitizer_report);

// The fatal signals' names, for the log ("SIGSEGV"); "signal N" for another.
[[nodiscard]] std::string signal_name(int signal);

}  // namespace fillscope
