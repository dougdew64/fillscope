// Replacing a file so that a reader never sees part of it: the viewer's context file, and the answer
// tool's answer file.
#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace fillscope {

// Replaces `file` with `text` atomically: written to a temporary file in the same directory, flushed
// to disk, then renamed over the old one. Rename within one filesystem is atomic on Linux, so a
// reader sees the whole old file or the whole new one, never part of either. Creates the file's
// directory if it has one and it doesn't exist. On failure, says what failed and why, and leaves no
// temporary file behind.
[[nodiscard]] std::expected<void, std::string> write_atomically(const std::filesystem::path& file,
                                                                std::string_view text);

}  // namespace fillscope
