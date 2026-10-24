#include "viewer/context_publisher.h"

#include "common/fd.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <format>
#include <system_error>

namespace fillscope {
namespace {

// The current errno's description. Read it immediately after the failing call.
std::string last_error() { return std::error_code{errno, std::system_category()}.message(); }

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Writes all of `text` to `fd`. write() may write less than asked, or be interrupted by a signal
// before writing anything (EINTR), so it is called until everything is written.
std::expected<void, std::string> write_all(const Fd& fd, std::string_view text) {
    while (!text.empty()) {
        const ssize_t written = ::write(fd.get(), text.data(), text.size());
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected{last_error()};
        }
        text.remove_prefix(static_cast<std::size_t>(written));
    }
    return {};
}

}  // namespace

std::expected<void, std::string> write_atomically(const std::filesystem::path& file, std::string_view text) {
    // A bare file name has no directory part: it goes in the working directory, which exists.
    if (const std::filesystem::path directory = file.parent_path(); !directory.empty()) {
        // The std::error_code overload reports failure through `error` instead of throwing.
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error) {
            return std::unexpected{std::format("can't create {}: {}", directory.string(), error.message())};
        }
    }
    // The process id keeps two writers' temporary files apart.
    std::filesystem::path temporary = file;
    temporary += std::format(".{}.tmp", ::getpid());

    // Removes the temporary file on every failure below, so none is left behind.
    const auto fail = [&](std::string reason) -> std::expected<void, std::string> {
        ::unlink(temporary.c_str());
        return std::unexpected{std::move(reason)};
    };
    {
        const Fd fd{::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)};
        if (!fd) {
            return fail(std::format("can't create {}: {}", temporary.string(), last_error()));
        }
        if (auto written = write_all(fd, text); !written) {
            return fail(std::format("can't write {}: {}", temporary.string(), written.error()));
        }
        // Flushed to disk before the rename, so after a crash the file is the old one or the new
        // one, never a renamed file whose contents never reached the disk.
        if (::fsync(fd.get()) != 0) {
            return fail(std::format("can't flush {}: {}", temporary.string(), last_error()));
        }
    }  // fd closes here
    if (::rename(temporary.c_str(), file.c_str()) != 0) {
        return fail(std::format("can't replace {}: {}", file.string(), last_error()));
    }
    return {};
}

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
        // unlink, not std::filesystem::remove, which would also remove an empty directory there.
        if (::unlink(file_.c_str()) == 0) {
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
