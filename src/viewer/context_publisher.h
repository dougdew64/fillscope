// Publishes the viewer's context (viewer/context.h) to a file the assistant reads, by default
// .fillscope/context.json in the viewer's working directory (viewer_options.h).
#pragma once

#include "common/atomic_file.h"
#include "viewer/context.h"

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace fillscope {

// Why a context is being published: the user did something, or something else changed (market
// data arrived). An enum rather than a bool, so a call reads update(context, PublishTrigger::user_action,
// now) instead of update(context, true, now), whose `true` says nothing at the call site.
enum class PublishTrigger { user_action, background };

class ContextPublisher {
public:
    using Clock = std::chrono::steady_clock;

    // `min_interval` limits how often a background change is written. This process is the file's
    // writer from construction on (Writer, in context.h).
    explicit ContextPublisher(std::filesystem::path file,
                              std::chrono::milliseconds min_interval = std::chrono::milliseconds{100});

    // Called once per paint with the current context. Writes it if it differs from what was last
    // published, or retries if the last write failed: at once after a user action, so the file is
    // current by the time the user can ask about it; otherwise at most once per min_interval. A
    // change held back is written by a later call once the interval has passed, so the file
    // catches up within min_interval plus one paint, if writing works.
    void update(const Context& context, PublishTrigger trigger, Clock::time_point now);

    // Called once, as the viewer exits: writes the context marked as from a writer that is no longer
    // running, so a reader can tell this file from a running viewer's.
    void finish(const Context& context);

    // What the file holds now, which is what the context bar draws: the last context written
    // successfully, never one that failed to write. When a write fails, the old file is removed if
    // it can be, so that a reader finds no file rather than one the user has since changed; then
    // nothing is published until a write succeeds.
    [[nodiscard]] const std::optional<Context>& published() const noexcept { return published_; }

    // The context bar's text for published(), built once per successful write rather than on every
    // paint.
    [[nodiscard]] const std::optional<ContextBarText>& published_bar() const noexcept { return published_bar_; }

    // Why the last write failed, or empty if it succeeded. Cleared by the next successful write.
    [[nodiscard]] const std::optional<std::string>& error() const noexcept { return error_; }

    // The sequence number of the last successful write by this writer.
    [[nodiscard]] std::uint64_t seq() const noexcept { return seq_; }

    [[nodiscard]] const Writer& writer() const noexcept { return writer_; }
    [[nodiscard]] const std::filesystem::path& file() const noexcept { return file_; }

private:
    void write(const Context& context);

    std::filesystem::path file_;
    std::chrono::milliseconds min_interval_;
    Writer writer_;
    std::optional<Context> published_;
    std::optional<ContextBarText> published_bar_;
    std::optional<std::string> error_;
    std::uint64_t seq_ = 0;
    std::optional<Clock::time_point> last_attempt_;
};

}  // namespace fillscope
