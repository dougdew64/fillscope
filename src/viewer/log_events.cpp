#include "viewer/log_events.h"

#include <format>
#include <variant>

namespace fillscope {

std::vector<std::string> log_events(const ViewerState& state, LogWatch& watch) {
    std::vector<std::string> lines;
    const BookBuilder& books = state.books;

    // Each rebuild from a snapshot, before the status it ends in, so the lines read in order.
    const auto recoveries = books.recoveries();
    for (std::size_t i = watch.recoveries; i < recoveries.size(); ++i) {
        const Recovery& recovery = recoveries[i];
        lines.push_back(std::format("feed: rebuilt from a snapshot as of update {}: the updates after {} up to it were never "
                                    "applied one by one ({})",
                                    recovery.snapshot_seq, recovery.after_seq, recovery.reason));
    }
    watch.recoveries = recoveries.size();

    if (books.status() != watch.status) {
        switch (books.status()) {
        case BookBuilder::Status::live:
            lines.push_back(std::format("feed: live, the books as of update {}", books.last_seq()));
            break;
        case BookBuilder::Status::recovering:
            lines.push_back(std::format("feed: recovering, after update {}: {}", books.last_seq(),
                                        books.recovering_because().value_or("no reason given")));
            break;
        case BookBuilder::Status::waiting:
            lines.push_back("feed: waiting for market data");
            break;
        }
        watch.status = books.status();
    }

    if (state.feed_error != watch.feed_error) {
        if (state.feed_error) {
            lines.push_back(std::format("feed: error receiving: {}", *state.feed_error));
        }
        watch.feed_error = state.feed_error;
    }

    if (state.answer) {
        const AnswerShown& shown = *state.answer;
        if (shown.problem != watch.answer_problem) {
            if (shown.problem) {
                lines.push_back(std::format("answer: refused {}: {}", shown.file.string(), *shown.problem));
            }
            watch.answer_problem = shown.problem;
        }
        if (shown.stamp != watch.answer_stamp) {
            if (shown.answer) {
                lines.push_back(std::format("answer: showing {}: {} steps, {} frames", shown.file.string(),
                                            shown.answer->scenario.steps.size(), shown.answer->frames.size()));
            }
            watch.answer_stamp = shown.stamp;
        }
    }
    return lines;
}

std::optional<std::string> describe(const ViewerActions& actions) {
    std::vector<std::string> what;
    if (actions.point_at) {
        if (const auto* order = std::get_if<OrderRef>(&*actions.point_at)) {
            what.push_back(std::format("points at order {} of ticker {}", order->order_id, order->ticker));
        } else {
            const auto& level = std::get<LevelRef>(*actions.point_at);
            what.push_back(std::format("points at the {} level at {} of ticker {}", to_string(level.side), level.price, level.ticker));
        }
    }
    if (actions.follow) {
        what.push_back(std::format("follows order {} of ticker {}", actions.follow->order_id, actions.follow->ticker));
    }
    if (actions.show_ticker) {
        what.push_back(std::format("shows ticker {}", *actions.show_ticker));
    }
    if (actions.replay_to) {
        what.push_back(std::format("goes to update {} of the replay", *actions.replay_to));
    }
    if (actions.answer_to) {
        what.push_back(std::format("goes to frame {} of the answer", *actions.answer_to));
    }
    if (actions.choose) {
        what.push_back(*actions.choose == 0 ? std::string{"chooses the latest question"}
                                            : std::format("chooses lesson {}", *actions.choose));
    }
    if (actions.clear_point) {
        what.push_back("stops pointing");
    }
    if (actions.clear_follow) {
        what.push_back("stops following");
    }
    if (what.empty()) {
        return std::nullopt;
    }
    std::string line = "user: ";
    for (std::size_t i = 0; i < what.size(); ++i) {
        line += (i == 0 ? "" : "; ") + what[i];
    }
    return line;
}

}  // namespace fillscope
