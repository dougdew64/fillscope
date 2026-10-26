// Where the viewer writes what it keeps (feed/kept_file.h), and its cleaning up after itself: a file
// of each run's own, `viewer-<pid>.kept` in the `kept` directory beside the context file, deleted
// when the viewer exits. A viewer that couldn't (killed outright, or crashed) leaves its file
// behind, and the next viewer to start removes those of viewers no longer running.
#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fillscope {

// A run's file's name, and the process id in one, if it is one.
[[nodiscard]] std::string kept_file_name(int pid);
[[nodiscard]] std::optional<int> kept_file_pid(std::string_view name);

// Removes from `directory` the kept files of viewers no longer running, and returns them. A file is a
// running viewer's, and kept, while a process with its id exists; one whose id has been taken by
// another process since waits for a later viewer. Nothing else in the directory is touched.
[[nodiscard]] std::vector<std::filesystem::path> remove_abandoned_kept_files(const std::filesystem::path& directory);

// Deletes the file it is given when it goes out of scope: on every way the viewer's main returns.
class DeleteOnExit {
public:
    explicit DeleteOnExit(std::filesystem::path file) : file_{std::move(file)} {}
    DeleteOnExit(const DeleteOnExit&) = delete;
    DeleteOnExit& operator=(const DeleteOnExit&) = delete;
    ~DeleteOnExit() {
        std::error_code ignored;
        std::filesystem::remove(file_, ignored);
    }

private:
    std::filesystem::path file_;
};

}  // namespace fillscope
