// The viewer's cleaning up of kept files (viewer/kept_files.h): the names, and removing at start only
// the files of viewers no longer running, never a running one's, and nothing else.
#include "viewer/kept_files.h"

#include <gtest/gtest.h>

#include <sys/wait.h>
#include <unistd.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <set>
#include <string>

namespace fillscope {
namespace {

TEST(KeptFiles, AreNamedForTheirViewersProcess) {
    EXPECT_EQ(kept_file_name(4321), "viewer-4321.kept");
    EXPECT_EQ(kept_file_pid("viewer-4321.kept"), 4321);
    for (const char* other : {"viewer-.kept", "viewer-12a.kept", "viewer-4321.log", "viewer-0.kept", "kept", "viewer-4321.kept.tmp",
                              "viewer-999999999999.kept"}) {
        EXPECT_FALSE(kept_file_pid(other)) << other;
    }
}

// A process id no process has: one that has just ended.
int a_finished_process() {
    const pid_t child = ::fork();
    if (child == 0) {
        ::_exit(0);
    }
    int status = 0;
    ::waitpid(child, &status, 0);
    return child;
}

TEST(KeptFiles, AStartingViewerRemovesOnlyThoseOfViewersNoLongerRunning) {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / std::format("fillscope-kept-files-{}", ::getpid());
    std::filesystem::create_directories(directory);
    const int gone = a_finished_process();
    for (const std::string& name : {kept_file_name(gone), kept_file_name(::getpid()), kept_file_name(::getppid()),
                                    std::string{"notes.txt"}, std::string{"viewer-x.kept"}}) {
        std::ofstream{directory / name} << "kept";
    }
    const auto removed = remove_abandoned_kept_files(directory);
    EXPECT_EQ(removed, (std::vector<std::filesystem::path>{directory / kept_file_name(gone)}));
    std::set<std::string> left;
    for (const auto& entry : std::filesystem::directory_iterator{directory}) {
        left.insert(entry.path().filename().string());
    }
    // A running viewer's (this process's, its parent's), and what isn't a kept file, stay.
    EXPECT_EQ(left, (std::set<std::string>{kept_file_name(::getpid()), kept_file_name(::getppid()), "notes.txt", "viewer-x.kept"}));
    std::filesystem::remove_all(directory);
}

}  // namespace
}  // namespace fillscope
