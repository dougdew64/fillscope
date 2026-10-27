#include "viewer/viewer_state.h"

#include <utility>

namespace fillscope {

Replay Replay::of(std::filesystem::path file, std::string recording, std::vector<MarketUpdate> updates) {
    // Update n is updates[n - 1]. An update the books can't apply leaves them recovering, which a
    // replay can't do (it has no snapshots): they stay where they are, and so does what is kept.
    BookBuilder books;
    for (const MarketUpdate& update : updates) {
        books.on_incremental(update);
        if (books.status() == BookBuilder::Status::recovering) {
            break;
        }
    }
    TimeShiftBuffer kept = TimeShiftBuffer::from_replay(updates);
    return Replay{.file = std::move(file),
                  .recording = std::move(recording),
                  .updates = std::move(updates),
                  .kept = std::move(kept),
                  .books = std::move(books),
                  .truth = std::nullopt,
                  .play = {}};
}

std::span<const OrderEvent> shown_history(const ViewerState& state, std::uint32_t ticker, std::uint64_t order_id) {
    const BookBuilder& feed = state.replay ? state.replay->books : state.live;
    return feed.history(ticker, order_id, state.books.last_seq());
}

}  // namespace fillscope
