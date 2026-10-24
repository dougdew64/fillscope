// The shared helpers in src/common: Fd closes the descriptor it owns, once, and parse_number reads
// a number only when the whole text is one.
#include "common/fd.h"
#include "common/parse.h"

#include <fcntl.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <string_view>
#include <utility>

namespace fillscope {
namespace {

// Whether `fd` is an open descriptor in this process.
bool is_open(int fd) { return ::fcntl(fd, F_GETFD) != -1; }

int open_null() { return ::open("/dev/null", O_RDONLY | O_CLOEXEC); }

TEST(Fd, StartsEmpty) {
    const Fd fd;
    EXPECT_EQ(fd.get(), -1);
    EXPECT_FALSE(fd);
}

TEST(Fd, ClosesOnDestruction) {
    const int raw = open_null();
    ASSERT_GE(raw, 0);
    {
        const Fd fd{raw};
        EXPECT_TRUE(fd);
        EXPECT_EQ(fd.get(), raw);
    }
    EXPECT_FALSE(is_open(raw));
}

TEST(Fd, ResetClosesAndEmpties) {
    const int raw = open_null();
    ASSERT_GE(raw, 0);
    Fd fd{raw};
    fd.reset();
    EXPECT_FALSE(is_open(raw));
    EXPECT_FALSE(fd);
}

TEST(Fd, AMoveLeavesTheSourceEmpty) {
    const int raw = open_null();
    ASSERT_GE(raw, 0);
    Fd source{raw};
    {
        const Fd moved{std::move(source)};
        EXPECT_EQ(moved.get(), raw);
        EXPECT_FALSE(source);  // NOLINT(bugprone-use-after-move): the moved-from state is the point
        EXPECT_TRUE(is_open(raw));
    }
    EXPECT_FALSE(is_open(raw));
}

TEST(Fd, MoveAssignmentClosesTheOldDescriptor) {
    const int first = open_null();
    const int second = open_null();
    ASSERT_GE(first, 0);
    ASSERT_GE(second, 0);
    Fd target{first};
    Fd source{second};
    target = std::move(source);
    EXPECT_FALSE(is_open(first));
    EXPECT_EQ(target.get(), second);
    EXPECT_FALSE(source);  // NOLINT(bugprone-use-after-move)
}

TEST(Fd, SelfMoveAssignmentKeepsTheDescriptor) {
    const int raw = open_null();
    ASSERT_GE(raw, 0);
    Fd fd{raw};
    Fd& same = fd;
    fd = std::move(same);
    EXPECT_EQ(fd.get(), raw);
    EXPECT_TRUE(is_open(raw));
}

TEST(ParseNumber, ReadsAWholeNumber) {
    EXPECT_EQ(parse_number<std::uint16_t>("20001"), 20001);
    EXPECT_EQ(parse_number<double>("2.5"), 2.5);
}

TEST(ParseNumber, RejectsTextThatIsNotExactlyANumber) {
    // Each of these std::stoul would accept, or read as something else.
    for (const char* bad : {"abc", "5x", "-1", "0x10", " 5", ""}) {
        EXPECT_FALSE(parse_number<std::uint32_t>(bad).has_value()) << "'" << bad << "'";
    }
    EXPECT_EQ(parse_number<std::uint32_t>("5x").error(), "'5x' is not a number");
}

TEST(ParseNumber, RejectsAValueTheTypeCantHold) {
    EXPECT_EQ(parse_number<std::uint16_t>("65536").error(), "'65536' is out of range");
    EXPECT_EQ(parse_number<std::uint16_t>("65535"), 65535);
}

// parse_number takes numbers only; bool is an integral type in C++ but has no from_chars. A concept
// asks whether a call compiles: outside a template, an invalid call would be an error, not false.
template <typename T>
concept Parsable = requires(std::string_view text) { parse_number<T>(text); };
static_assert(Parsable<int> && Parsable<double> && !Parsable<bool>);

}  // namespace
}  // namespace fillscope
