#include "answer/wire.h"

#include <bit>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <string_view>
#include <type_traits>
#include <variant>

namespace fillscope {

// The files are little-endian, and their fields are copied as they are.
static_assert(std::endian::native == std::endian::little, "the back end's files are little-endian");

namespace {

constexpr std::string_view kRecordingMagic = "LLREQ001";
constexpr std::string_view kStepsMagic = "LLSTEP01";
constexpr std::uint8_t kNew = 1, kCancel = 2;  // ClientRequestType

// Writes `value`'s bytes at `at` and returns the position after them.
template <typename T>
std::size_t put(std::byte* bytes, std::size_t at, T value) noexcept {
    std::memcpy(bytes + at, &value, sizeof value);
    return at + sizeof value;
}

// Reads a little-endian field from a byte string that has been checked to hold it.
template <typename T>
T get(const std::string& bytes, std::size_t at) noexcept {
    T value;
    std::memcpy(&value, bytes.data() + at, sizeof value);  // unaligned: copied, never cast
    return value;
}

ClientResponse decode_response(const std::string& bytes, std::size_t at) noexcept {
    return ClientResponse{.type = static_cast<ResponseType>(get<std::uint8_t>(bytes, at)),
                          .client = get<std::uint32_t>(bytes, at + 1),
                          .ticker = get<std::uint32_t>(bytes, at + 5),
                          .client_order_id = get<std::uint64_t>(bytes, at + 9),
                          .market_order_id = get<std::uint64_t>(bytes, at + 17),
                          .side = static_cast<Side>(get<std::int8_t>(bytes, at + 25)),
                          .price = get<std::int64_t>(bytes, at + 26),
                          .exec_qty = get<std::uint32_t>(bytes, at + 34),
                          .leaves_qty = get<std::uint32_t>(bytes, at + 38)};
}

void encode_response(const ClientResponse& r, std::byte* bytes) noexcept {
    std::size_t at = put(bytes, 0, static_cast<std::uint8_t>(r.type));
    at = put(bytes, at, r.client);
    at = put(bytes, at, r.ticker);
    at = put(bytes, at, r.client_order_id);
    at = put(bytes, at, r.market_order_id);
    at = put(bytes, at, static_cast<std::int8_t>(r.side));
    at = put(bytes, at, r.price);
    at = put(bytes, at, r.exec_qty);
    put(bytes, at, r.leaves_qty);
}

}  // namespace

RequestBytes encode_request(const Request& request, std::uint32_t ticker) noexcept {
    RequestBytes bytes{};
    // std::visit calls the overload for whichever alternative the variant holds; `if constexpr`
    // picks the branch at compile time, from the alternative's type.
    std::visit(
        [&](const auto& action) {
            using Action = std::decay_t<decltype(action)>;
            const bool is_new = std::is_same_v<Action, NewOrder>;
            std::size_t at = put(bytes.data(), 0, is_new ? kNew : kCancel);
            at = put(bytes.data(), at, request.client);
            at = put(bytes.data(), at, ticker);
            at = put(bytes.data(), at, action.id);
            if constexpr (std::is_same_v<Action, NewOrder>) {
                at = put(bytes.data(), at, static_cast<std::int8_t>(action.side));
                at = put(bytes.data(), at, action.price);
                put(bytes.data(), at, action.qty);
            } else {
                at = put(bytes.data(), at, static_cast<std::int8_t>(Side::none));
                at = put(bytes.data(), at, std::int64_t{0});
                put(bytes.data(), at, std::uint32_t{0});
            }
        },
        request.action);
    return bytes;
}

std::expected<void, std::string> write_recording(const std::filesystem::path& path, const std::vector<Request>& requests,
                                                 std::uint32_t ticker) {
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    if (!out) {
        return std::unexpected{std::format("can't create {}", path.string())};
    }
    out.write(kRecordingMagic.data(), static_cast<std::streamsize>(kRecordingMagic.size()));
    for (const Request& request : requests) {
        const RequestBytes bytes = encode_request(request, ticker);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    out.close();
    if (!out) {
        return std::unexpected{std::format("can't write {}", path.string())};
    }
    return {};
}

std::expected<StepsFile, std::string> read_steps_file(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return std::unexpected{std::format("can't open {}", path.string())};
    }
    const std::string bytes{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    if (!bytes.starts_with(kStepsMagic)) {
        return std::unexpected{std::format("{} isn't a replay's steps (it doesn't start with {})", path.string(), kStepsMagic)};
    }
    const auto cut_short = [&] { return std::unexpected{std::format("{} is cut short", path.string())}; };
    std::size_t at = kStepsMagic.size();
    if (bytes.size() < at + 4) {
        return cut_short();
    }
    const auto length = get<std::uint32_t>(bytes, at);
    at += 4;
    if (bytes.size() - at < length) {
        return cut_short();
    }
    StepsFile file{.recording = bytes.substr(at, length), .steps = {}};
    at += length;
    constexpr std::size_t kHead = kRequestSize + 8 + 4 + 4;  // the request, first update, count, responses
    while (at < bytes.size()) {
        if (bytes.size() - at < kHead) {
            return cut_short();
        }
        ReplayStep step;
        std::memcpy(step.request.data(), bytes.data() + at, kRequestSize);
        step.first_update = get<std::uint64_t>(bytes, at + kRequestSize);
        step.update_count = get<std::uint32_t>(bytes, at + kRequestSize + 8);
        const auto responses = get<std::uint32_t>(bytes, at + kRequestSize + 12);
        at += kHead;
        if ((bytes.size() - at) / kResponseSize < responses) {
            return cut_short();
        }
        for (std::uint32_t i = 0; i < responses; ++i, at += kResponseSize) {
            const auto type = get<std::uint8_t>(bytes, at);
            if (type < static_cast<std::uint8_t>(ResponseType::accepted) || type > static_cast<std::uint8_t>(ResponseType::rejected)) {
                return std::unexpected{std::format("{}: step {} has a response of type {}, which isn't one", path.string(),
                                                   file.steps.size() + 1, type)};
            }
            step.responses.push_back(decode_response(bytes, at));
        }
        file.steps.push_back(std::move(step));
    }
    return file;
}

std::vector<std::byte> encode_steps_file(const StepsFile& file) {
    std::vector<std::byte> bytes(kStepsMagic.size() + 4 + file.recording.size());
    std::memcpy(bytes.data(), kStepsMagic.data(), kStepsMagic.size());
    put(bytes.data(), kStepsMagic.size(), static_cast<std::uint32_t>(file.recording.size()));
    std::memcpy(bytes.data() + kStepsMagic.size() + 4, file.recording.data(), file.recording.size());
    for (const ReplayStep& step : file.steps) {
        const std::size_t at = bytes.size();
        bytes.resize(at + kRequestSize + 16 + step.responses.size() * kResponseSize);
        std::memcpy(bytes.data() + at, step.request.data(), kRequestSize);
        std::size_t next = put(bytes.data(), at + kRequestSize, step.first_update);
        next = put(bytes.data(), next, step.update_count);
        next = put(bytes.data(), next, static_cast<std::uint32_t>(step.responses.size()));
        for (const ClientResponse& response : step.responses) {
            encode_response(response, bytes.data() + next);
            next += kResponseSize;
        }
    }
    return bytes;
}

}  // namespace fillscope
