#include "viewer/context_publisher.h"

#include <unistd.h>

#include <cerrno>
#include <format>

namespace fillscope {
namespace {

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

ContextPublisher::ContextPublisher(std::filesystem::path file, std::chrono::milliseconds min_interval)
    : file_{std::move(file)},
      min_interval_{min_interval},
      writer_{.pid = ::getpid(), .started_at_ms = now_ms(), .running = true} {}

void ContextPublisher::update(const Context& context, PublishTrigger trigger, Clock::time_point now) {
    if (published_ == context && !error_) {
        return;  // the file already says exactly this
    }
    if (trigger == PublishTrigger::background && last_attempt_ && now - *last_attempt_ < min_interval_) {
        return;  // a background change, too soon after the last write; a later call will write it
    }
    last_attempt_ = now;
    write(context);
}

void ContextPublisher::finish(const Context& context) {
    writer_.running = false;
    write(context);
}

void ContextPublisher::write(const Context& context) {
    const std::string text = to_json(context, writer_, seq_ + 1, now_ms()).dump(1) + "\n";
    if (auto written = write_atomically(file_, text); !written) {
        error_ = std::move(written.error());  // the bar shows this
        // The old file no longer says what the user has assembled. Removing it is often impossible
        // too (when the directory can't be written), and then published_ still says what it holds.
        // But if there is no file there (ENOENT), or a directory has taken its place (EISDIR),
        // nothing holds a context. unlink, not std::filesystem::remove, which would also remove an
        // empty directory there.
        if (::unlink(file_.c_str()) == 0 || errno == ENOENT || errno == EISDIR) {
            published_.reset();
            published_bar_.reset();
        }
        return;
    }
    ++seq_;
    published_ = context;
    published_bar_ = bar_text(context);
    error_.reset();
}

}  // namespace fillscope
