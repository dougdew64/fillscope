// Every relative link in the repository's Markdown documents resolves to a file or directory that
// exists. Nothing else reads the documents' links, so without this a broken one fails silently.
//
// FILLSCOPE_DOC_LIST names a file listing the documents, one path per line (written by
// tests/docs/CMakeLists.txt).

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace fillscope {
namespace {

namespace fs = std::filesystem;

// The relative link targets in a Markdown text, [text](target), without any #fragment. Web links,
// mail links and links within the page (#section) are left out, and so is anything inside a fenced
// code block, which is shown as code rather than followed.
std::vector<std::string> relative_links(const std::string& markdown) {
    static const std::regex link{R"(\]\(([^)\s#]*)(?:#[^)\s]*)?\))"};
    std::vector<std::string> targets;
    bool in_code_block = false;
    std::istringstream lines{markdown};
    for (std::string line; std::getline(lines, line);) {
        const auto first = line.find_first_not_of(' ');
        if (first != std::string::npos && line.compare(first, 3, "```") == 0) {
            in_code_block = !in_code_block;
            continue;
        }
        if (in_code_block) {
            continue;
        }
        for (auto it = std::sregex_iterator(line.begin(), line.end(), link); it != std::sregex_iterator(); ++it) {
            const std::string target = (*it)[1];
            if (target.empty() || target.starts_with("http://") || target.starts_with("https://") ||
                target.starts_with("mailto:")) {
                continue;
            }
            targets.push_back(target);
        }
    }
    return targets;
}

std::vector<fs::path> documents() {
    std::vector<fs::path> paths;
    std::ifstream list{FILLSCOPE_DOC_LIST};
    for (std::string line; std::getline(list, line);) {
        if (!line.empty()) {
            paths.emplace_back(line);
        }
    }
    return paths;
}

std::string read_file(const fs::path& path) {
    std::ifstream file{path};
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

TEST(DocLinks, FindsRelativeLinksOnly) {
    const std::string markdown =
        "See [a](docs/A.md), [up](../B.md), [a section](C.md#part) and [a folder](src/).\n"
        "Not [the web](https://example.com), [mail](mailto:x@example.com) or [here](#top).\n"
        "```sh\n"
        "[in code](not-a-link.md)\n"
        "```\n";
    EXPECT_EQ(relative_links(markdown), (std::vector<std::string>{"docs/A.md", "../B.md", "C.md", "src/"}));
}

TEST(DocLinks, ChecksTheRepositorysDocuments) {
    // A list that missed the documents would make the next test pass while checking nothing.
    const auto paths = documents();
    auto listed = [&](const fs::path& tail) {
        return std::ranges::any_of(paths, [&](const fs::path& p) { return p.string().ends_with(tail.string()); });
    };
    EXPECT_TRUE(listed("/README.md"));
    EXPECT_TRUE(listed("/docs/CHARTER.md"));
    EXPECT_TRUE(listed("/docs/RUNNING.md"));
}

TEST(DocLinks, EveryRelativeLinkResolves) {
    std::vector<std::string> broken;
    for (const fs::path& document : documents()) {
        for (const std::string& target : relative_links(read_file(document))) {
            if (!fs::exists(document.parent_path() / target)) {
                broken.push_back(document.filename().string() + ": " + target);
            }
        }
    }
    std::string report;
    for (const auto& entry : broken) {
        report += "\n  " + entry;
    }
    EXPECT_TRUE(broken.empty()) << "broken links:" << report;
}

}  // namespace
}  // namespace fillscope
