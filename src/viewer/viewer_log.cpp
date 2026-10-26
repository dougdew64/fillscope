#include "viewer/viewer_log.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <format>
#include <system_error>
#include <vector>

namespace fillscope {
namespace {

bool all_digits(std::string_view s) {
    return !s.empty() && std::ranges::all_of(s, [](char c) { return c >= '0' && c <= '9'; });
}

}  // namespace

std::string log_file_name(ViewerLog::WallClock::time_point started, int pid) {
    return std::format("viewer-{:%Y%m%d-%H%M%S}-{}.log", std::chrono::floor<std::chrono::seconds>(started), pid);
}

bool is_log_file_name(std::string_view name) {
    // "viewer-" 8 digits "-" 6 digits "-" digits ".log"
    constexpr std::string_view kPrefix = "viewer-";
    constexpr std::string_view kSuffix = ".log";
    if (!name.starts_with(kPrefix) || !name.ends_with(kSuffix)) {
        return false;
    }
    const std::string_view middle = name.substr(kPrefix.size(), name.size() - kPrefix.size() - kSuffix.size());
    return middle.size() > 16 && all_digits(middle.substr(0, 8)) && middle[8] == '-' && all_digits(middle.substr(9, 6)) &&
           middle[15] == '-' && all_digits(middle.substr(16));
}

std::expected<ViewerLog, std::string> ViewerLog::open(const std::filesystem::path& directory, WallClock::time_point started,
                                                      int pid) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        return std::unexpected{std::format("can't make the log directory {}: {}", directory.string(), error.message())};
    }
    // The logs already there, oldest first; all but the newest kKeptRuns - 1 go, with the files beside
    // each that its run's sanitizers wrote (its name, then more).
    std::vector<std::string> logs;
    std::vector<std::string> others;
    for (const auto& entry : std::filesystem::directory_iterator{directory, error}) {
        const std::string name = entry.path().filename().string();
        (is_log_file_name(name) ? logs : others).push_back(name);
    }
    if (error) {
        return std::unexpected{std::format("can't read the log directory {}: {}", directory.string(), error.message())};
    }
    std::ranges::sort(logs);
    const std::size_t old = logs.size() >= kKeptRuns ? logs.size() - (kKeptRuns - 1) : 0;
    for (std::size_t i = 0; i < old; ++i) {
        std::filesystem::remove(directory / logs[i], error);
        for (const std::string& other : others) {
            if (other.starts_with(logs[i])) {
                std::filesystem::remove(directory / other, error);
            }
        }
    }

    const std::filesystem::path path = directory / log_file_name(started, pid);
    // O_EXCL: a run's log is never another's, nor appended to.
    Fd fd{::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_APPEND | O_CLOEXEC, 0644)};
    if (!fd) {
        return std::unexpected{std::format("can't create the log {}: {}", path.string(), std::strerror(errno))};
    }
    return ViewerLog{std::move(fd), path};
}

bool ViewerLog::write_whole(std::string_view text) {
    while (!text.empty()) {
        const ssize_t written = ::write(fd_.get(), text.data(), text.size());
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            error_ = std::format("can't write the log {}: {}", path_.string(), std::strerror(errno));
            return false;
        }
        bytes_ += static_cast<std::uintmax_t>(written);
        text.remove_prefix(static_cast<std::size_t>(written));
    }
    return true;
}

bool ViewerLog::line(std::string_view text) {
    if (full_ || !fd_) {
        return false;
    }
    const auto wall = std::chrono::floor<std::chrono::milliseconds>(WallClock::now());
    const double since = std::chrono::duration<double>(Clock::now() - opened_).count();
    std::string out = std::format("{:%Y-%m-%dT%H:%M:%S}Z +{:.3f}s ", wall, since);
    // The text between its line breaks copied whole, each break written as " / ".
    for (std::size_t at = 0; at <= text.size();) {
        const std::size_t end = std::min(text.find('\n', at), text.size());
        out.append(text.substr(at, end - at));
        if (end < text.size()) {
            out += " / ";
        }
        at = end + 1;
    }
    out += '\n';
    // The limit is on what the run writes; the line saying it was reached comes after it.
    if (bytes_ + out.size() > kMaxBytes) {
        full_ = true;
        (void)write_whole(std::format("{:%Y-%m-%dT%H:%M:%S}Z +{:.3f}s the log has reached its limit, {} bytes: nothing more "
                                      "is written to it\n",
                                      wall, since, kMaxBytes));
        return false;
    }
    return write_whole(out);
}

}  // namespace fillscope
