// The course (course/): every lesson played through the real answer tool and exchange_replay, and
// read back as the viewer reads it, every claim checked again; and the manifest naming exactly the
// lessons course/ holds. So a lesson that stops being true, because the back end or the tool
// changed, fails here rather than being shown.
//
//   course_system_tests FILLSCOPE_ANSWER COURSE_DIR
#include "answer/course.h"
#include "support/test_support.h"
#include "viewer/answer_state.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <format>
#include <set>
#include <string>

namespace fillscope {
namespace {

std::string g_answer_tool;
std::filesystem::path g_course_dir;

TEST(Course, TheManifestNamesEveryLessonOnce) {
    const auto course = read_course_file(g_course_dir / "course.json");
    ASSERT_TRUE(course) << course.error();
    std::set<std::string> named;
    for (const Lesson& lesson : course->lessons) {
        named.insert(lesson.scenario);
    }
    std::set<std::string> present;
    for (const auto& entry : std::filesystem::directory_iterator{g_course_dir}) {
        if (entry.path().extension() == ".json" && entry.path().filename() != "course.json") {
            present.insert(entry.path().filename().string());
        }
    }
    EXPECT_EQ(named, present) << "a lesson in course/ the manifest doesn't name, or one it names that isn't there";
}

// Each lesson as the build plays it, and as the viewer reads it: every claim true by the viewer's own
// check, not only the tool's.
TEST(Course, EveryLessonPlaysAndEveryClaimHolds) {
    const auto course = read_course_file(g_course_dir / "course.json");
    ASSERT_TRUE(course) << course.error();
    const std::filesystem::path built = std::filesystem::temp_directory_path() / std::format("fillscope-course-{}", ::getpid());
    std::filesystem::remove_all(built);
    for (const Lesson& lesson : course->lessons) {
        const std::filesystem::path answer = lesson_answer(built, lesson);
        const auto result = test::run_command(std::format("{} {} --answer {}", test::shell_quote(g_answer_tool),
                                                          test::shell_quote((g_course_dir / lesson.scenario).string()),
                                                          test::shell_quote(answer.string())));
        ASSERT_EQ(result.status, 0) << lesson.scenario << ": " << result.output;
        const auto loaded = load_answer(answer);
        ASSERT_TRUE(loaded) << lesson.scenario << ": " << loaded.error();
        for (std::size_t i = 0; i < loaded->checked.size(); ++i) {
            for (const ClaimResult& claim : loaded->checked[i].claims) {
                EXPECT_TRUE(claim.holds) << lesson.scenario << ", step " << i + 1 << ": " << claim.says << " In fact, " << claim.actually;
            }
        }
    }
    std::filesystem::remove_all(built);
}

}  // namespace
}  // namespace fillscope

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    if (argc != 3) {
        std::fprintf(stderr, "usage: course_system_tests FILLSCOPE_ANSWER COURSE_DIR\n");
        return 2;
    }
    fillscope::g_answer_tool = argv[1];
    fillscope::g_course_dir = argv[2];
    return RUN_ALL_TESTS();
}
