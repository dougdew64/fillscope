#include "feed/kept_file.h"

#include <fcntl.h>
#include <unistd.h>

#include <bit>
#include <cerrno>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <span>
#include <vector>

namespace fillscope {
namespace {

static_assert(std::endian::native == std::endian::little, "the file's numbers are written as the machine holds them");

constexpr char kRunTag = 'R';
constexpr char kUpdateTag = 'U';
// An update with no arrival time (a replay's): never written by the viewer, whose updates all have one.
constexpr std::int64_t kNoTime = INT64_MIN;

template <typename T>
void put(std::string& out, T value) {
    out.append(reinterpret_cast<const char*>(&value), sizeof value);
}

void put_update(std::string& out, const MarketUpdate& update) {
    const auto bytes = encode_update(update);
    out.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::string run_record(const Moment& base) {
    std::string out{kRunTag};
    put(out, base.seq);
    std::uint32_t orders = 0;
    for (const Book& book : base.books) {
        orders += static_cast<std::uint32_t>(book.order_count());
    }
    put(out, orders);
    for (std::uint32_t ticker = 0; ticker < BookBuilder::kTickers; ++ticker) {
        for (const Order& order : base.books[ticker].orders()) {
            put_update(out, MarketUpdate{.seq = base.seq, .type = UpdateType::add, .order_id = order.order_id, .ticker = ticker,
                                         .side = order.side, .price = order.price, .qty = order.qty, .priority = order.priority});
        }
    }
    std::uint8_t trades = 0;
    for (const auto& trade : base.last_trades) {
        trades += trade ? 1 : 0;
    }
    put(out, trades);
    for (const auto& trade : base.last_trades) {
        if (trade) {
            put_update(out, *trade);
        }
    }
    return out;
}

std::string update_record(const MarketUpdate& update, std::optional<TimeShiftBuffer::Clock::time_point> arrived) {
    std::string out{kUpdateTag};
    put_update(out, update);
    put(out, arrived ? static_cast<std::int64_t>(arrived->time_since_epoch().count()) : kNoTime);
    return out;
}

// Reads a file's bytes from `at`, stopping cleanly where they run out: `need` says whether `size`
// more are there.
class Reader {
public:
    explicit Reader(std::span<const std::byte> bytes) : bytes_{bytes} {}
    [[nodiscard]] bool need(std::size_t size) const { return at_ + size <= bytes_.size(); }
    template <typename T>
    T get() {
        T value;
        std::memcpy(&value, bytes_.data() + at_, sizeof value);
        at_ += sizeof value;
        return value;
    }
    std::span<const std::byte> take(std::size_t size) {
        const auto taken = bytes_.subspan(at_, size);
        at_ += size;
        return taken;
    }
    [[nodiscard]] std::size_t at() const { return at_; }

private:
    std::span<const std::byte> bytes_;
    std::size_t at_ = 0;
};

std::expected<MarketUpdate, std::string> one_update(std::span<const std::byte> bytes) {
    auto decoded = decode_updates(bytes);
    if (!decoded) {
        return std::unexpected{decoded.error()};
    }
    return decoded->front();
}

}  // namespace

std::expected<KeptFileWriter, std::string> KeptFileWriter::create(const std::filesystem::path& path) {
    Fd fd{::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644)};
    if (!fd) {
        return std::unexpected{std::format("can't create {}: {}", path.string(), std::strerror(errno))};
    }
    KeptFileWriter writer{std::move(fd), path};
    if (auto written = writer.write_record(std::string{kKeptFileMagic}); !written) {
        return std::unexpected{written.error()};
    }
    return writer;
}

std::expected<void, std::string> KeptFileWriter::write_record(const std::string& record) {
    std::string_view left = record;
    while (!left.empty()) {
        const ssize_t written = ::write(fd_.get(), left.data(), left.size());
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected{std::format("can't write {}: {}", path_.string(), std::strerror(errno))};
        }
        left.remove_prefix(static_cast<std::size_t>(written));
    }
    return {};
}

std::expected<void, std::string> KeptFileWriter::write_new(const TimeShiftBuffer& kept) {
    const auto& runs = kept.runs();
    for (std::size_t run = runs_written_ == 0 ? 0 : runs_written_ - 1; run < runs.size(); ++run) {
        if (run >= runs_written_) {
            const auto base = kept.moment(runs[run].first);
            if (!base) {
                return std::unexpected{base.error()};
            }
            if (auto written = write_record(run_record(*base)); !written) {
                return written;
            }
            runs_written_ = run + 1;
            last_written_ = runs[run].first;
        }
        for (std::uint64_t seq = last_written_ + 1; seq <= runs[run].last; ++seq) {
            if (auto written = write_record(update_record(*kept.update(seq), kept.arrived(seq))); !written) {
                return written;
            }
            last_written_ = seq;
        }
    }
    return {};
}

std::expected<TimeShiftBuffer, std::string> read_kept_file(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return std::unexpected{std::format("can't open {}", path.string())};
    }
    const std::vector<char> chars{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    const std::span<const std::byte> bytes = std::as_bytes(std::span{chars});
    if (bytes.size() < kKeptFileMagic.size() ||
        std::string_view{chars.data(), kKeptFileMagic.size()} != kKeptFileMagic) {
        return std::unexpected{std::format("{} isn't a kept file (it doesn't begin with {})", path.string(), kKeptFileMagic)};
    }
    Reader reader{bytes.subspan(kKeptFileMagic.size())};
    TimeShiftBuffer kept;
    // A record cut short at the end is one still being written: what is before it is the file.
    while (reader.need(1)) {
        const auto tag = static_cast<char>(reader.get<std::byte>());
        if (tag == kRunTag) {
            if (!reader.need(sizeof(std::uint64_t) + sizeof(std::uint32_t))) {
                break;
            }
            Moment base;
            base.seq = reader.get<std::uint64_t>();
            const auto orders = reader.get<std::uint32_t>();
            if (!reader.need(std::size_t{orders} * kUpdateSize + 1)) {
                break;
            }
            for (std::uint32_t i = 0; i < orders; ++i) {
                const auto order = one_update(reader.take(kUpdateSize));
                if (!order || order->ticker >= BookBuilder::kTickers || !base.books[order->ticker].apply(*order)) {
                    return std::unexpected{std::format("{}: a run's order at byte {} isn't one", path.string(), reader.at())};
                }
            }
            const auto trades = reader.get<std::uint8_t>();
            if (!reader.need(std::size_t{trades} * kUpdateSize)) {
                break;
            }
            for (std::uint8_t i = 0; i < trades; ++i) {
                const auto trade = one_update(reader.take(kUpdateSize));
                if (!trade || trade->ticker >= BookBuilder::kTickers) {
                    return std::unexpected{std::format("{}: a run's last trade at byte {} isn't one", path.string(), reader.at())};
                }
                base.last_trades[trade->ticker] = *trade;
            }
            if (auto started = kept.start_run(std::move(base)); !started) {
                return std::unexpected{std::format("{}: {}", path.string(), started.error())};
            }
        } else if (tag == kUpdateTag) {
            if (!reader.need(kUpdateSize + sizeof(std::int64_t))) {
                break;
            }
            const auto update = one_update(reader.take(kUpdateSize));
            const auto time = reader.get<std::int64_t>();
            if (!update) {
                return std::unexpected{std::format("{}: an update at byte {} isn't one: {}", path.string(), reader.at(), update.error())};
            }
            const std::optional<TimeShiftBuffer::Clock::time_point> arrived =
                time == kNoTime ? std::nullopt : std::optional{TimeShiftBuffer::Clock::time_point{TimeShiftBuffer::Clock::duration{time}}};
            if (auto appended = kept.append(*update, arrived); !appended) {
                return std::unexpected{std::format("{}: {}", path.string(), appended.error())};
            }
        } else {
            return std::unexpected{std::format("{}: byte {} begins no record", path.string(), reader.at() - 1 + kKeptFileMagic.size())};
        }
    }
    return kept;
}

}  // namespace fillscope
