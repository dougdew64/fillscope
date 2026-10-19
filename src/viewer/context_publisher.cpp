#include "viewer/context_publisher.h"

#include <unistd.h>

#include <format>
#include <fstream>
#include <system_error>

namespace fillscope {

std::expected<void, std::string> write_atomically(const std::filesystem::path& file, std::string_view text) {
    // The std::error_code overloads report failure through `error` instead of throwing, which
    // suits a function whose callers expect failure as a value.
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    if (error) {
        return std::unexpected{std::format("can't create {}: {}", file.parent_path().string(), error.message())};
    }
    // The process id keeps two writers' temporary files apart.
    std::filesystem::path temporary = file;
    temporary += std::format(".{}.tmp", ::getpid());
    {
        std::ofstream out{temporary, std::ios::binary | std::ios::trunc};
        out << text;
        out.close();  // closed here rather than at the end of the scope, so a failed flush is seen
        if (!out) {
            std::filesystem::remove(temporary, error);
            return std::unexpected{std::format("can't write {}", temporary.string())};
        }
    }
    std::filesystem::rename(temporary, file, error);
    if (error) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return std::unexpected{std::format("can't replace {}: {}", file.string(), error.message())};
    }
    return {};
}

void ContextPublisher::update(const Context& context, bool user_action, Clock::time_point now) {
    if (published_ == context && !error_) {
        return;  // the file already says exactly this
    }
    if (!user_action && last_attempt_ && now - *last_attempt_ < min_interval_) {
        return;  // a background change, too soon after the last write; a later call will write it
    }
    last_attempt_ = now;

    const auto written_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count();
    const std::string text = to_json(context, seq_ + 1, written_at_ms).dump(1) + "\n";
    if (auto written = write_atomically(file_, text); !written) {
        error_ = std::move(written.error());  // the bar shows this; published_ still says what the file holds
        return;
    }
    ++seq_;
    published_ = context;
    error_.reset();
}

}  // namespace fillscope
