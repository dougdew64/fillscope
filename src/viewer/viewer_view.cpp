#include "viewer/viewer_view.h"

#include <imgui.h>
#include <imgui_internal.h>  // IMGUI_TEST_ENGINE_ITEM_INFO, ImGuiItemStatusFlags

#include <concepts>
#include <cstddef>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace fillscope {
namespace {

constexpr ImVec4 kErrorColor{1.0f, 0.4f, 0.4f, 1.0f};
constexpr ImVec4 kFollowedRowColor{0.45f, 0.38f, 0.10f, 0.65f};
constexpr ImVec4 kBidColor{0.45f, 0.85f, 0.45f, 1.0f};
constexpr ImVec4 kAskColor{0.95f, 0.50f, 0.45f, 1.0f};

// Draws text that is already formatted. TextUnformatted takes the characters as they are, with no
// printf-style % processing, so a '%' in the text can't be misread as a format directive.
void text(std::string_view s) { ImGui::TextUnformatted(s.data(), s.data() + s.size()); }

// A tooltip for the item just drawn, while it is hovered. `describe` builds the text, and is called
// only while hovered, so no tooltip text is built for the items nobody is pointing at.
// std::invocable<F> is a C++20 concept: it accepts anything callable with no arguments.
template <std::invocable F>
void tooltip(F&& describe) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
        const std::string what = describe();
        ImGui::SetTooltip("%s", what.c_str());
    }
}

// Tells the Dear ImGui Test Engine, when one is built in, whether the item just drawn is marked
// (pointed at, followed, the ticker shown), as Dear ImGui does for a checkbox's tick. Dear ImGui
// doesn't report a Selectable's selected state or a row's colour, so without this the UI tests could
// check only the rule (level_marked and the rest), not the book as drawn. In the viewer itself the
// macro is empty.
void report_marked([[maybe_unused]] const Label& label, [[maybe_unused]] bool marked) {
    ImGuiContext& g = *ImGui::GetCurrentContext();  // the macro refers to the context as `g`
    IMGUI_TEST_ENGINE_ITEM_INFO(ImGui::GetItemID(), label.c_str(),
                                ImGui::GetItemStatusFlags() | ImGuiItemStatusFlags_Checkable |
                                    (marked ? ImGuiItemStatusFlags_Checked : 0));
}

// What a click does, for a tooltip: sent at once, unless writes are failing.
std::string_view sends(const std::optional<std::string>& publish_error) {
    return publish_error ? "but publishing is failing (see the context bar), so the assistant won't receive it"
                         : "sends it to the assistant now";
}

// The context bar: what the assistant has been given, drawn from the last published context and
// never from the live selection, so the bar and the file can't disagree.
// A heading on its own line, and its parts indented beneath it, so the background reads as part
// of the context rather than as the heading's continuation.
void draw_context_bar(const ViewerView& view, ViewerActions& actions) {
    text(kContextHeading);
    ImGui::Indent();
    if (!view.published_bar) {
        text(view.publish_error ? "nothing published" : "nothing published yet");
    } else {
        const ContextBarText& bar = *view.published_bar;
        text(bar.background);
        if (bar.point) {
            text(*bar.point);
            ImGui::SameLine();
            if (ImGui::SmallButton(kClearPointButton)) {
                actions.clear_point = true;
            }
            tooltip([&] { return std::format("Stop pointing at it: {}", sends(view.publish_error)); });
        }
        if (bar.follow) {
            text(*bar.follow);
            ImGui::SameLine();
            if (ImGui::SmallButton(kClearFollowButton)) {
                actions.clear_follow = true;
            }
            tooltip([&] { return std::format("Stop following it: {}", sends(view.publish_error)); });
        }
    }
    if (view.publish_error) {
        // Push and pop must pair; the style stack is checked at the end of the frame.
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
        text(std::format("NOT published: {}", *view.publish_error));
        ImGui::PopStyleColor();
    }
    ImGui::Unindent();
}

// The market data's state and the viewer's own liveness.
void draw_status(const ViewerView& view) {
    const ViewerState& state = view.state;
    const BookBuilder& books = state.books;
    if (state.replay) {
        // A replay is read from a file: not the exchange, and with no snapshots to recover from.
        if (books.status() == BookBuilder::Status::recovering) {
            text(std::format("market data: a replay, stuck after update {}: {}; a replay has no snapshots to recover from",
                             books.last_seq(), books.recovering_because().value_or("an update can't be applied")));
        } else {
            text(std::format("market data: a replay, the books are as of update {}", books.last_seq()));
        }
    } else {
        switch (books.status()) {
        case BookBuilder::Status::waiting:
            text("market data: waiting for the exchange");
            break;
        case BookBuilder::Status::live:
            text(std::format("market data: live, the books are as of update {}", books.last_seq()));
            break;
        case BookBuilder::Status::recovering:
            text(std::format("market data: recovering, the books are as of update {}: {}; waiting for a snapshot",
                             books.last_seq(), books.recovering_because().value_or("updates were missed")));
            break;
        }
    }
    text(std::format("updates received: {}  recoveries: {}", books.updates_received(), books.recoveries().size()));
    if (state.feed_error) {
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
        text(std::format("last market data error: {}", *state.feed_error));
        ImGui::PopStyleColor();
    }
    if (view.timing.seconds_since_update) {
        text(std::format("seconds since last update: {:.1f}", *view.timing.seconds_since_update));
    }
    text(std::format("viewer paints: {} (keeps rising while the exchange is stopped)", view.timing.paints));
}

// A replay's position and controls. Each control asks for the books after one update; the loop
// rebuilds them (ViewerApp::replay_to).
void draw_replay(const ViewerView& view, ViewerActions& actions) {
    const Replay& replay = *view.state.replay;
    const std::uint64_t at = view.state.books.last_seq();
    const std::uint64_t last = replay.updates.size();
    text(std::format("replay of {} (recorded in {}): after update {} of {}", replay.file.string(), replay.recording, at, last));
    if (ImGui::SmallButton(kReplayStart)) {
        actions.replay_to = 0;
    }
    tooltip([] { return std::string{"To the start: the books before the first update"}; });
    ImGui::SameLine();
    if (ImGui::SmallButton(kReplayBack) && at > 0) {
        actions.replay_to = at - 1;
    }
    tooltip([] { return std::string{"Back one update"}; });
    ImGui::SameLine();
    if (ImGui::SmallButton(kReplayForward) && at < last) {
        actions.replay_to = at + 1;
    }
    tooltip([] { return std::string{"Forward one update"}; });
    ImGui::SameLine();
    if (ImGui::SmallButton(kReplayEnd)) {
        actions.replay_to = last;
    }
    tooltip([] { return std::string{"To the end: the books after the last update"}; });
    ImGui::SameLine();
    std::uint64_t position = at;
    const std::uint64_t first = 0;
    if (ImGui::SliderScalar(kReplaySlider, ImGuiDataType_U64, &position, &first, &last) && position != at) {
        actions.replay_to = position;
    }
}

// One button per ticker, its order count in its tooltip; the shown one is marked.
void draw_tickers(const ViewerView& view, ViewerActions& actions) {
    text("ticker");
    for (std::uint32_t ticker = 0; ticker < BookBuilder::kTickers; ++ticker) {
        ImGui::SameLine();
        const bool shown = ticker == view.state.shown_ticker;
        const Label label = ticker_label(ticker);
        if (ImGui::Selectable(label.c_str(), shown, ImGuiSelectableFlags_None, ImVec2{ImGui::GetFontSize() * 1.5f, 0.0f})) {
            actions.show_ticker = ticker;
        }
        report_marked(label, shown);
        tooltip([&] {
            return std::format("Show ticker {}'s book ({}): {}", ticker,
                               view.state.books.book(ticker).order_count() == 1
                                   ? std::string{"1 order"}
                                   : std::format("{} orders", view.state.books.book(ticker).order_count()),
                               sends(view.publish_error));
        });
    }
}

// One row of the book: an order, and whether it is the first of its level.
struct BookRow {
    const Level* level;
    std::size_t position;  // in its level's queue, from 0
};

// The shown ticker's book, one row per resting order, asks above bids, each side's best price
// nearest the middle; within a price, orders in the order they will fill. Clicking a row points at
// its order; clicking a price points at its level; clicking an order's id follows it. What is pointed
// at and followed is highlighted as published, like the bar.
void draw_book(const ViewerView& view, ViewerActions& actions) {
    const std::uint32_t ticker = view.state.shown_ticker;
    const Book& book = view.state.books.book(ticker);
    if (book.order_count() == 0) {
        text(std::format("ticker {}: no orders", ticker));
        return;
    }
    // Built every paint: a book holds at most a few hundred levels (the exchange's limit), so this
    // costs little beside drawing them.
    std::vector<Level> asks = book.levels(Side::sell);
    std::vector<Level> bids = book.levels(Side::buy);
    std::vector<BookRow> rows;
    for (auto level = asks.rbegin(); level != asks.rend(); ++level) {  // worst ask at the top
        for (std::size_t i = 0; i < level->orders.size(); ++i) {
            rows.push_back({&*level, i});
        }
    }
    for (const Level& level : bids) {  // best bid at the top of the bids
        for (std::size_t i = 0; i < level.orders.size(); ++i) {
            rows.push_back({&level, i});
        }
    }

    constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                      ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
    // Half the remaining height; the followed order's history takes the rest.
    const float height = view.published && view.published->follow ? ImGui::GetContentRegionAvail().y * 0.6f
                                                                  : ImGui::GetContentRegionAvail().y;
    if (!ImGui::BeginTable(kBookTable, 6, flags, ImVec2{0.0f, height})) {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);  // the header row stays visible while scrolling
    ImGui::TableSetupColumn("side");
    ImGui::TableSetupColumn("price");
    ImGui::TableSetupColumn("level qty");
    ImGui::TableSetupColumn("queue");
    ImGui::TableSetupColumn("order");
    ImGui::TableSetupColumn("qty");
    ImGui::TableHeadersRow();

    for (const BookRow& row : rows) {
        const Level& level = *row.level;
        const Order& order = level.orders[row.position];
        const OrderRef order_ref{ticker, order.order_id};
        const LevelRef level_ref{ticker, level.side, level.price};
        ImGui::TableNextRow();
        const bool followed = order_followed(order_ref, view.published);
        if (followed) {
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(kFollowedRowColor));
        }

        ImGui::TableNextColumn();
        // SpanAllColumns makes the whole row the click target; AllowOverlap lets the price's and the
        // id's buttons, drawn on top of it, take their own clicks.
        ImGui::PushStyleColor(ImGuiCol_Text, level.side == Side::buy ? kBidColor : kAskColor);
        const Label row_label = order_label(order);
        const bool pointed = order_pointed(order_ref, view.published);
        if (ImGui::Selectable(row_label.c_str(), pointed, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
            actions.point_at = order_ref;
        }
        ImGui::PopStyleColor();
        report_marked(row_label, pointed);
        tooltip([&] { return std::format("Point at order {}: {}", order.order_id, sends(view.publish_error)); });

        ImGui::TableNextColumn();
        if (row.position == 0) {  // the level's price and total, on its first row
            const Label price_label = level_label(level_ref);
            if (ImGui::SmallButton(price_label.c_str())) {
                actions.point_at = level_ref;
            }
            report_marked(price_label, level_marked(level_ref, view.published));
            tooltip([&] {
                return std::format("Point at the {} level at {}: {}", to_string(level.side), level.price, sends(view.publish_error));
            });
            ImGui::TableNextColumn();
            text(Label{"{}", level.qty}.c_str());
        } else {
            ImGui::TableNextColumn();
        }

        ImGui::TableNextColumn();
        text(Label{"#{}", row.position + 1}.c_str());
        ImGui::TableNextColumn();
        const Label id_label = follow_label(order);
        if (ImGui::SmallButton(id_label.c_str())) {
            actions.follow = order_ref;
        }
        report_marked(id_label, followed);
        tooltip([&] {
            return std::format("Follow order {}: every update to it, and {}", order.order_id, sends(view.publish_error));
        });
        ImGui::TableNextColumn();
        text(Label{"{}", order.qty}.c_str());
    }
    ImGui::EndTable();
}

// The followed order's history, as published.
void draw_history(const Context::Follow& follow) {
    text(std::format("order {} of ticker {}: {} event{}", follow.order.ref.order_id, follow.order.ref.ticker,
                     follow.event_count, follow.event_count == 1 ? "" : "s"));
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                      ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable(kHistoryTable, 6, flags, ImVec2{0.0f, ImGui::GetContentRegionAvail().y})) {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("update");
    ImGui::TableSetupColumn("event");
    ImGui::TableSetupColumn("side");
    ImGui::TableSetupColumn("price");
    ImGui::TableSetupColumn("qty");
    ImGui::TableSetupColumn("seen");
    ImGui::TableHeadersRow();
    for (const OrderEvent& event : follow.events) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        text(Label{"{}", event.seq}.c_str());
        ImGui::TableNextColumn();
        text(to_string(event.type));
        ImGui::TableNextColumn();
        text(to_string(event.side));
        ImGui::TableNextColumn();
        text(Label{"{}", event.price}.c_str());
        ImGui::TableNextColumn();
        text(Label{"{}", event.qty}.c_str());
        ImGui::TableNextColumn();
        text(event.from_snapshot ? "in a snapshot" : "as it happened");
    }
    ImGui::EndTable();
}

}  // namespace

void apply(const ViewerActions& actions, Selection& selection, ViewerState& state) {
    if (actions.show_ticker && *actions.show_ticker < BookBuilder::kTickers) {
        state.shown_ticker = *actions.show_ticker;
    }
    if (actions.point_at) {
        selection.point_at(*actions.point_at);
    }
    if (actions.follow) {
        selection.follow(*actions.follow);
    }
    if (actions.clear_point) {
        selection.clear_point();
    }
    if (actions.clear_follow) {
        selection.clear_follow();
    }
}

bool level_marked(const LevelRef& level, const std::optional<Context>& published) {
    if (!published || !published->point) {
        return false;
    }
    const auto* state = std::get_if<LevelState>(&published->point->target);
    return state && state->ref == level;
}

bool order_pointed(const OrderRef& order, const std::optional<Context>& published) {
    if (!published || !published->point) {
        return false;
    }
    const auto* state = std::get_if<OrderState>(&published->point->target);
    return state && state->ref == order;
}

bool order_followed(const OrderRef& order, const std::optional<Context>& published) {
    return published && published->follow && published->follow->order.ref == order;
}

Label ticker_label(std::uint32_t ticker) { return Label{"{}##ticker", ticker}; }

Label level_label(const LevelRef& level) { return Label{"{}##level{}", level.price, to_string(level.side)}; }

Label order_label(const Order& order) { return Label{"{}##order{}", to_string(order.side), order.order_id}; }

Label follow_label(const Order& order) { return Label{"{}##follow", order.order_id}; }

ViewerActions draw_viewer(const ViewerView& view) {
    ViewerActions actions;
    ImGui::SetNextWindowPos({0.0f, 0.0f});
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin(kViewerWindow, nullptr, ImGuiWindowFlags_NoDecoration);

    draw_context_bar(view, actions);
    ImGui::Separator();
    draw_status(view);
    if (view.state.replay) {
        draw_replay(view, actions);
    }
    ImGui::Separator();
    draw_tickers(view, actions);
    draw_book(view, actions);
    if (view.published && view.published->follow) {
        ImGui::Separator();
        draw_history(*view.published->follow);
    }

    ImGui::End();
    return actions;
}

}  // namespace fillscope
