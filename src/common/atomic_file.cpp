#include "common/atomic_file.h"

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

}  // namespace fillscope
