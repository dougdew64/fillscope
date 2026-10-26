// The file the viewer writes what it keeps to (feed/time_shift.h), as it keeps it, so a live moment
// has an address: the file and an update number, from which the books then are rebuilt exactly,
// however much later, while the viewer runs (docs/ARCHITECTURE.md, §3, *Keeping what arrives*).
//
// The file: 8 bytes, "FSKEEP01"; then records, each written with one write(2), so a reader of a file
// still being written takes every whole record and leaves a last one cut short:
//   - a run's start, 'R': the update it starts at (8 bytes), its resting orders (a 4-byte count,
//     then each as an add update), and each ticker's last trade (a 1-byte count, then each as a
//     trade update). The updates before it, across a gap, aren't in the file, so its books are.
//   - an update, 'U': the update, and when it arrived (8 bytes: the viewer's steady clock, in
//     nanoseconds; only differences between them mean anything).
// Numbers are little-endian; an update is its 42 bytes on the wire (market_update.h).
#pragma once

#include "common/fd.h"
#include "feed/time_shift.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace fillscope {

inline constexpr std::string_view kKeptFileMagic = "FSKEEP01";

// Writes what a TimeShiftBuffer keeps to a file, appending what it has kept since the last write.
class KeptFileWriter {
public:
    // A new file at `path` (made, or emptied), with its header; or why it can't be.
    [[nodiscard]] static std::expected<KeptFileWriter, std::string> create(const std::filesystem::path& path);

    // Appends what `kept` has kept since the last call: each new run's start and each new update.
    [[nodiscard]] std::expected<void, std::string> write_new(const TimeShiftBuffer& kept);

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    KeptFileWriter(Fd fd, std::filesystem::path path) : fd_{std::move(fd)}, path_{std::move(path)} {}
    [[nodiscard]] std::expected<void, std::string> write_record(const std::string& record);

    Fd fd_;
    std::filesystem::path path_;
    std::size_t runs_written_ = 0;      // the runs whose start has been written
    std::uint64_t last_written_ = 0;    // the last update written, in the last run written
};

// What a kept file holds, as a TimeShiftBuffer: its runs and updates, with their times, up to its
// last whole record; or why it can't be read (no file, not a kept file, a record that isn't one).
[[nodiscard]] std::expected<TimeShiftBuffer, std::string> read_kept_file(const std::filesystem::path& path);

}  // namespace fillscope
