#include "answer/course.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <fstream>
#include <set>

namespace fillscope {

std::expected<Course, std::string> read_course(const nlohmann::json& json) {
    if (!json.is_object() || !json.contains("title") || !json["title"].is_string() || !json.contains("lessons") ||
        !json["lessons"].is_array() || json["lessons"].empty()) {
        return std::unexpected{"the course needs a 'title' and a list of 'lessons', at least one"};
    }
    for (const auto& [key, value] : json.items()) {
        if (key != "title" && key != "lessons") {
            return std::unexpected{std::format("the course has a field '{}', and may have only 'title' and 'lessons'", key)};
        }
    }
    Course course{.title = json["title"].get<std::string>(), .lessons = {}};
    std::set<std::string> scenarios;
    for (std::size_t i = 0; i < json["lessons"].size(); ++i) {
        const nlohmann::json& lesson = json["lessons"][i];
        const std::string where = std::format("lesson {}", i + 1);
        if (!lesson.is_object()) {
            return std::unexpected{std::format("{}: must be an object", where)};
        }
        for (const auto& [key, value] : lesson.items()) {
            if ((key != "title" && key != "scenario" && key != "read") || !value.is_string()) {
                return std::unexpected{std::format("{}: '{}' isn't one of its fields (title, scenario, read), as text", where, key)};
            }
        }
        const std::string title = lesson.value("title", "");
        const std::string scenario = lesson.value("scenario", "");
        if (title.empty() || scenario.empty()) {
            return std::unexpected{std::format("{}: needs a 'title' and a 'scenario'", where)};
        }
        // A plain file name in course/, so a lesson can't name a file elsewhere.
        // One dot only, before "json": the build names the lesson's directory by the name up to its first
        // dot, and the viewer by the name without its last extension, which must agree.
        if (scenario.find('/') != std::string::npos || !scenario.ends_with(".json") || scenario == ".json" ||
            scenario == "course.json" || std::ranges::count(scenario, '.') != 1) {
            return std::unexpected{std::format("{}: its scenario '{}' must be a .json file's name in course/", where, scenario)};
        }
        if (!scenarios.insert(scenario).second) {
            return std::unexpected{std::format("{}: its scenario '{}' is another lesson's too", where, scenario)};
        }
        course.lessons.push_back({.title = title, .scenario = scenario, .read = lesson.value("read", "")});
    }
    return course;
}

std::expected<Course, std::string> read_course_file(const std::filesystem::path& file) {
    std::ifstream in{file};
    if (!in) {
        return std::unexpected{std::format("there is no course at {}", file.string())};
    }
    const nlohmann::json json = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (json.is_discarded()) {
        return std::unexpected{std::format("{} isn't valid JSON", file.string())};
    }
    return read_course(json);
}

std::filesystem::path lesson_answer(const std::filesystem::path& built, const Lesson& lesson) {
    return built / std::filesystem::path{lesson.scenario}.stem() / "answer.json";
}

}  // namespace fillscope
