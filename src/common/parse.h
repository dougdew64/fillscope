// Strict parsing of numbers from text, as on a command line.
//
// std::stoi and its relatives are the wrong tool for this: they throw on bad input, skip leading
// whitespace, read "5x" as 5 and "0x10" as 0, and std::stoull reads "-1" as 18446744073709551615.
// std::from_chars (C++17) does none of that. It never throws, never allocates, ignores the locale,
// and reports exactly how many characters it used, so "the whole text was one number" can be
// checked.
#pragma once

#include <charconv>
#include <concepts>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <system_error>

namespace fillscope {

// The number `text` spells, or why it isn't one. All of the text must be the number: no sign
// where the type has none, no leading or trailing characters, and a value the type can hold.
// `std::integral<T> || std::floating_point<T>` is a C++20 constraint: the template accepts only
// arithmetic types, and a call with any other type fails to compile with that reason.
template <typename T>
    requires std::integral<T> || std::floating_point<T>
[[nodiscard]] std::expected<T, std::string> parse_number(std::string_view text) {
    T value{};
    const char* const first = text.data();
    const char* const last = text.data() + text.size();
    // A structured binding unpacks from_chars's result: where it stopped, and the error, if any.
    const auto [stopped_at, error] = std::from_chars(first, last, value);
    if (error == std::errc::result_out_of_range) {
        return std::unexpected{std::format("'{}' is out of range", text)};
    }
    if (error != std::errc{} || stopped_at != last) {
        return std::unexpected{std::format("'{}' is not a number", text)};
    }
    return value;
}

}  // namespace fillscope
