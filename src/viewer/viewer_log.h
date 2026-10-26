// The viewer's log: what it did and saw, a file per run, so a failure can be diagnosed after it
// happened, a crash included (docs/ARCHITECTURE.md, *The viewer's log*).
//
// Each line is written as it happens, in one write(2) to a file opened for appending, so it is in the
// kernel's hands at once: a crash of the viewer loses nothing written before it (only the machine
// going down could). The logs are kept when the viewer exits; they are bounded instead: a starting
// viewer keeps only the newest kKeptRuns, and a run's log stops at kMaxBytes, saying so.
#pragma once

#include "common/fd.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace fillscope {

class ViewerLog {
public:
    using WallClock = std::chrono::system_clock;
    using Clock = std::chrono::steady_clock;

    static constexpr std::size_t kKeptRuns = 20;
    static constexpr std::uintmax_t kMaxBytes = 10 * 1024 * 1024;

    // A new log in `directory`, made if missing, named for `started` and `pid` (log_file_name),
    // after deleting all but the newest kKeptRuns - 1 logs already there, with the files a run's
    // sanitizers wrote beside its log; nothing else in the directory is touched. Or why it can't.
    [[nodiscard]] static std::expected<ViewerLog, std::string> open(const std::filesystem::path& directory,
                                                                    WallClock::time_point started, int pid);

    // Writes one line: the time (UTC, to the millisecond), the seconds since the log was opened, and
    // `text`, whose own line breaks are kept inside the line as " / ". Once the log has reached
    // kMaxBytes, a last line says so, and nothing more is written. Returns whether `text` was
    // written; a failed write is remembered (error()).
    bool line(std::string_view text);

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    // The descriptor, for the crash handlers (crash_log.h), which write to it directly.
    [[nodiscard]] int fd() const noexcept { return fd_.get(); }
    [[nodiscard]] std::uintmax_t bytes() const noexcept { return bytes_; }
    [[nodiscard]] bool full() const noexcept { return full_; }
    // Why the last write failed, if one did.
    [[nodiscard]] const std::string& error() const noexcept { return error_; }

private:
    ViewerLog(Fd fd, std::filesystem::path path) : fd_{std::move(fd)}, path_{std::move(path)} {}
    bool write_whole(std::string_view text);

    Fd fd_;
    std::filesystem::path path_;
    Clock::time_point opened_ = Clock::now();
    std::uintmax_t bytes_ = 0;
    bool full_ = false;
    std::string error_;
};

// A run's log's name: "viewer-YYYYMMDD-HHMMSS-<pid>.log", the start in UTC. Names sort as the runs
// started (to the second; then by process id).
[[nodiscard]] std::string log_file_name(ViewerLog::WallClock::time_point started, int pid);

// Whether `name` is a log's name, as log_file_name makes them.
[[nodiscard]] bool is_log_file_name(std::string_view name);

}  // namespace fillscope
