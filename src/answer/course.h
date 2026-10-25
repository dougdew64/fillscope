// The course (course/ in the repository): lessons, each an answer's scenario (answer/scenario.h)
// stored and tested rather than written for one question, in the order they are meant to be taken.
// Its manifest, course/course.json, lists them; the build plays each through the answer tool into
// the build's course/ directory, and the viewer offers them in its answer pane.
#pragma once

#include <nlohmann/json_fwd.hpp>

#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace fillscope {

struct Lesson {
    std::string title;
    std::string scenario;  // its scenario's file name in course/, such as "01-limit-orders.json"
    std::string read;      // what to read alongside it, if anything is named

    friend bool operator==(const Lesson&, const Lesson&) = default;
};

struct Course {
    std::string title;
    std::vector<Lesson> lessons;
};

// The course the manifest describes, or why it can't be read: no lessons, a title missing, a
// scenario named twice or by anything but a plain .json file name, or a field it doesn't have.
[[nodiscard]] std::expected<Course, std::string> read_course(const nlohmann::json& json);
[[nodiscard]] std::expected<Course, std::string> read_course_file(const std::filesystem::path& file);

// Where the build writes a lesson's answer: `built`/<the scenario's name without .json>/answer.json.
[[nodiscard]] std::filesystem::path lesson_answer(const std::filesystem::path& built, const Lesson& lesson);

}  // namespace fillscope
