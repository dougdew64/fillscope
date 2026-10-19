// Publishes the viewer's context (viewer/context.h) to a file the assistant reads, by default
// .fillscope/context.json in the viewer's working directory.
#pragma once

#include "viewer/context.h"

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace fillscope {

inline constexpr std::string_view kDefaultContextFile = ".fillscope/context.json";

// Replaces `file` with `text` atomically: written to a temporary file in the same directory, then
// renamed over the old one. Rename within one filesystem is atomic on Linux, so a reader sees the
// whole old file or the whole new one, never half of either. Creates the directory if needed.
[[nodiscard]] std::expected<void, std::string> write_atomically(const std::filesystem::path& file,
                                                                std::string_view text);

class ContextPublisher {
public:
    using Clock = std::chrono::steady_clock;

    // `min_interval` limits how often a change the user didn't make (frames arriving) is written.
    explicit ContextPublisher(std::filesystem::path file,
                              std::chrono::milliseconds min_interval = std::chrono::milliseconds{100})
        : file_{std::move(file)}, min_interval_{min_interval} {}

    // Called once per paint with the current context. Writes it if it differs from what was last
    // published: at once after a user action, so the file is current by the time the user can ask
    // about it; otherwise at most once per min_interval. A change held back is written by a later
    // call once the interval has passed, so the file always catches up.
    void update(const Context& context, bool user_action, Clock::time_point now);

    // What the file holds now, which is what the context bar draws: the last context written
    // successfully, never one that failed to write.
    [[nodiscard]] const std::optional<Context>& published() const noexcept { return published_; }

    // Why the last write failed, or empty if it succeeded. Cleared by the next successful write.
    [[nodiscard]] const std::optional<std::string>& error() const noexcept { return error_; }

    // The sequence number of the last successful write.
    [[nodiscard]] std::uint64_t seq() const noexcept { return seq_; }

    [[nodiscard]] const std::filesystem::path& file() const noexcept { return file_; }

private:
    std::filesystem::path file_;
    std::chrono::milliseconds min_interval_;
    std::optional<Context> published_;
    std::optional<std::string> error_;
    std::uint64_t seq_ = 0;
    std::optional<Clock::time_point> last_attempt_;
};

}  // namespace fillscope
