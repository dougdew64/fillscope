// The back end's files an answer is made from, as bytes: the recording of requests the answer tool
// writes (backend/Chapter12/exchange/matcher/request_recording.h), and the steps file its replay
// writes (exchange_replay --steps, matcher/replay.h there). Written and read here from their
// layouts, as the market data is (feed/market_update.h), rather than by including the back end's
// headers: a second, independent reading of the formats, which the back end's tests pin down.
#pragma once

#include "answer/scenario.h"
#include "feed/market_update.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace fillscope {

// A request on the wire, MEClientRequest: type (1 byte), client (4), ticker (4), the client's order
// id (8), side (1), price (8), quantity (4). Packed, little-endian.
inline constexpr std::size_t kRequestSize = 30;
// A response, MEClientResponse: type (1), client (4), ticker (4), the client's order id (8), the
// exchange's order id (8), side (1), price (8), executed quantity (4), quantity left (4).
inline constexpr std::size_t kResponseSize = 42;

using RequestBytes = std::array<std::byte, kRequestSize>;

// The back end's response types, with their values on the wire.
enum class ResponseType : std::uint8_t { accepted = 1, canceled = 2, filled = 3, cancel_rejected = 4, rejected = 5 };

// A field the back end leaves unset holds its type's largest value (its *_INVALID).
inline constexpr std::uint32_t kNoQty = 0xFFFF'FFFF;
inline constexpr std::uint64_t kNoOrderId = 0xFFFF'FFFF'FFFF'FFFF;

struct ClientResponse {
    ResponseType type = ResponseType::accepted;
    std::uint32_t client = 0;
    std::uint32_t ticker = 0;
    std::uint64_t client_order_id = 0;
    std::uint64_t market_order_id = 0;  // the exchange's id, as in the market data; kNoOrderId if none
    Side side = Side::none;
    std::int64_t price = 0;
    std::uint32_t exec_qty = 0;    // a fill's quantity; kNoQty if not a fill
    std::uint32_t leaves_qty = 0;  // what remains of the order; kNoQty if unknown

    friend bool operator==(const ClientResponse&, const ClientResponse&) = default;
};

// What one request did, as the replay says: its bytes, the market updates it made (numbered as in
// the market data) and the responses sent to clients for it, in order.
struct ReplayStep {
    RequestBytes request{};
    std::uint64_t first_update = 0;  // its first update's number, or the next one's if it made none
    std::uint32_t update_count = 0;
    std::vector<ClientResponse> responses;
};

struct StepsFile {
    std::string recording;  // the recording it was replayed from, as exchange_replay was given it
    std::vector<ReplayStep> steps;
};

// The request's bytes, as a client sends it to the exchange. A cancel carries no side, price or
// quantity: the matcher finds the order by its client and id.
[[nodiscard]] RequestBytes encode_request(const Request& request, std::uint32_t ticker) noexcept;

// Writes `requests` on `ticker` as a recording: 8 bytes, "LLREQ001", then each request's bytes.
[[nodiscard]] std::expected<void, std::string> write_recording(const std::filesystem::path& path,
                                                               const std::vector<Request>& requests, std::uint32_t ticker);

// The steps file's contents, or why it can't be read: no file, not a steps file, cut short, or a
// response of an unknown type.
[[nodiscard]] std::expected<StepsFile, std::string> read_steps_file(const std::filesystem::path& path);

// A steps file's bytes, as the replay writes them, for tests.
[[nodiscard]] std::vector<std::byte> encode_steps_file(const StepsFile& file);

}  // namespace fillscope
