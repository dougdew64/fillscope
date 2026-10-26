// fillscope-books: the books exactly as they were after one update of a file the viewer wrote what it
// keeps to (feed/kept_file.h), at a live moment's address the context file gives (keptAddress).
//
//   fillscope-books FILE --at SEQ [--ticker T]
//
// Prints which update, and when it arrived beside the newest kept; then the books (books_text.h).
// Exits 1, saying why, if the file can't be read or the update can't be shown (in a gap, before the
// first kept, after the newest); 2 for a command line it can't read.

#include "common/parse.h"
#include "feed/books_text.h"
#include "feed/kept_file.h"

#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <print>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view kUsage = "usage: fillscope-books FILE --at SEQ [--ticker T]";

}  // namespace

int main(int argc, char** argv) {
    std::optional<std::string_view> file;
    std::optional<std::uint64_t> at;
    std::optional<std::uint32_t> ticker;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--at" && i + 1 < argc) {
            const auto number = fillscope::parse_number<std::uint64_t>(argv[++i]);
            if (!number) {
                std::println(stderr, "fillscope-books: --at: {}\n{}", number.error(), kUsage);
                return 2;
            }
            at = *number;
        } else if (arg == "--ticker" && i + 1 < argc) {
            const auto number = fillscope::parse_number<std::uint32_t>(argv[++i]);
            if (!number || *number >= fillscope::BookBuilder::kTickers) {
                std::println(stderr, "fillscope-books: --ticker takes 0 to {}\n{}", fillscope::BookBuilder::kTickers - 1, kUsage);
                return 2;
            }
            ticker = *number;
        } else if (!arg.starts_with("--") && !file) {
            file = arg;
        } else {
            std::println(stderr, "fillscope-books: {} isn't an option\n{}", arg, kUsage);
            return 2;
        }
    }
    if (!file || !at) {
        std::println(stderr, "{}", kUsage);
        return 2;
    }
    const auto kept = fillscope::read_kept_file(*file);
    if (!kept) {
        std::println(stderr, "fillscope-books: {}", kept.error());
        return 1;
    }
    const auto moment = kept->moment(*at);
    if (!moment) {
        std::println(stderr, "fillscope-books: {}", moment.error());
        return 1;
    }
    std::string when;
    const auto newest = kept->newest().value_or(*at);
    const auto arrived = kept->arrived(*at);
    const auto newest_arrived = kept->arrived(newest);
    if (arrived && newest_arrived) {
        when = std::format("; it arrived {:.3f} s before the newest kept, update {}", std::chrono::duration<double>(*newest_arrived - *arrived).count(), newest);
    } else {
        when = std::format("; the newest kept is update {}", newest);
    }
    std::print("the books after update {}{}\n{}", *at, when, fillscope::books_text(*moment, ticker));
    return 0;
}
