#include "viewer/kept_files.h"

#include <signal.h>

#include <cerrno>
#include <format>

namespace fillscope {

std::string kept_file_name(int pid) { return std::format("viewer-{}.kept", pid); }

std::optional<int> kept_file_pid(std::string_view name) {
    constexpr std::string_view kPrefix = "viewer-";
    constexpr std::string_view kSuffix = ".kept";
    if (!name.starts_with(kPrefix) || !name.ends_with(kSuffix) || name.size() <= kPrefix.size() + kSuffix.size()) {
        return std::nullopt;
    }
    const std::string_view digits = name.substr(kPrefix.size(), name.size() - kPrefix.size() - kSuffix.size());
    int pid = 0;
    for (const char c : digits) {
        if (c < '0' || c > '9' || pid > 100'000'000) {
            return std::nullopt;
        }
        pid = pid * 10 + (c - '0');
    }
    return pid > 0 ? std::optional{pid} : std::nullopt;
}

std::vector<std::filesystem::path> remove_abandoned_kept_files(const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> removed;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator{directory, error}) {
        const auto pid = kept_file_pid(entry.path().filename().string());
        // kill with no signal asks only whether the process exists: ESRCH, none does.
        if (pid && ::kill(*pid, 0) != 0 && errno == ESRCH) {
            std::error_code ignored;
            if (std::filesystem::remove(entry.path(), ignored)) {
                removed.push_back(entry.path());
            }
        }
    }
    return removed;
}

}  // namespace fillscope
