// The viewer's UI, tested with the Dear ImGui Test Engine on its null backend: real Dear ImGui
// frames and real clicks, with no window and no GPU.
//
// Each test runs the viewer's loop through ViewerApp, the class src/viewer/main.cpp runs it through,
// so the tests take exactly the program's steps in the program's order: take in market data, draw,
// apply what the user did, publish the context (to a scratch file per test).
//
// What the viewer shows is checked two ways. Its text is captured with Dear ImGui's own logging,
// which records every piece of text actually rendered: plain Text() items have no ID, so the Test
// Engine can't query them. Items with an ID (rows, buttons) are found and clicked through the Test
// Engine; "**/" in a reference searches the whole window for an item with that label.
//
//   viewer_ui_tests [FILTER]     e.g. "^viewer/clicking" (Test Engine filter syntax)
//
// Exits 0 only if at least one test ran and every test that ran passed.

#include "feed/feed_socket.h"
#include "feed/market_update.h"
#include "support/test_support.h"
#include "viewer/context.h"
#include "viewer/viewer_app.h"
#include "viewer/viewer_view.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_te_context.h>
#include <imgui_te_engine.h>
#include <imgui_app.h>
#include <nlohmann/json.hpp>

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <deque>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using fillscope::MarketUpdate;
using fillscope::Side;
using fillscope::UpdateType;
using namespace std::chrono_literals;

// Each test's scratch files live under here; removed when the program ends.
std::filesystem::path scratch_root() {
    return std::filesystem::temp_directory_path() / std::format("fillscope-viewer-ui-{}", ::getpid());
}

struct UiVars {
    std::unique_ptr<fillscope::ViewerApp> app;  // made before the test's first frame
    // Set only when a test receives real market data.
    std::optional<fillscope::FeedSocket> incremental;
    std::optional<fillscope::FeedSocket> snapshot;
    std::string text;  // the text the viewer rendered last frame
};

// Runs before the test's first frame: a viewer publishing to this test's own scratch file.
void set_up(ImGuiTestContext* ctx, UiVars& vars) {
    const std::filesystem::path dir = scratch_root() / ctx->Test->Name;
    std::filesystem::remove_all(dir);
    vars.app = std::make_unique<fillscope::ViewerApp>(dir / "context.json");
}

bool contains(const std::string& text, std::string_view part) { return text.find(part) != std::string::npos; }

// The context file as it is now.
nlohmann::json read_context(const UiVars& vars) {
    std::ifstream in{vars.app->publisher().file()};
    return nlohmann::json::parse(in);
}

// Whether the item `ref` was drawn marked (pointed at, followed, the ticker shown): viewer_view.cpp
// reports it to the Test Engine as a checkbox's tick.
bool marked(ImGuiTestContext* ctx, const char* ref) {
    return (ctx->ItemInfo(ref).StatusFlags & ImGuiItemStatusFlags_Checked) != 0;
}

// Where an item was drawn, from the top of the window.
float top_of(ImGuiTestContext* ctx, const char* ref) { return ctx->ItemInfo(ref).RectFull.Min.y; }

// A path every write to fails: a directory can't be created inside a regular file.
std::filesystem::path unwritable() {
    const std::filesystem::path blocker = scratch_root() / "not-a-directory";
    std::filesystem::create_directories(scratch_root());
    std::ofstream{blocker} << "x";
    return blocker / "context.json";
}

MarketUpdate add(std::uint64_t seq, std::uint64_t id, Side side, std::int64_t price, std::uint32_t qty, std::uint64_t priority,
                 std::uint32_t ticker = 0) {
    return {.seq = seq, .type = UpdateType::add, .order_id = id, .ticker = ticker, .side = side, .price = price, .qty = qty, .priority = priority};
}

// Ticker 0's book, from updates 1 to 6: asks 10 at 102, then 11 and 12 at 101; bids 1 and 2 at 100,
// then 3 at 99. On screen, top to bottom: 10, 11, 12, 1, 2, 3.
void add_book(UiVars& vars) {
    const auto now = std::chrono::steady_clock::now();
    for (const MarketUpdate& update : {add(1, 10, Side::sell, 102, 3, 1), add(2, 11, Side::sell, 101, 2, 1),
                                       add(3, 12, Side::sell, 101, 4, 2), add(4, 1, Side::buy, 100, 5, 1),
                                       add(5, 2, Side::buy, 100, 7, 2), add(6, 3, Side::buy, 99, 4, 1)}) {
        vars.app->apply_incremental(update, now);
    }
}

// One frame of the viewer's loop, as in src/viewer/main.cpp, with the rendered text captured.
void viewer_frame(ImGuiTestContext* ctx) {
    UiVars& vars = ctx->GetVars<UiVars>();
    const auto now = std::chrono::steady_clock::now();
    if (vars.incremental && vars.snapshot) {
        vars.app->receive(*vars.incremental, *vars.snapshot, now);
    }
    ImGui::LogToBuffer();
    (void)vars.app->paint(now);
    vars.text = ImGui::GetCurrentContext()->LogBuffer.c_str();
    ImGui::LogFinish();
}

// Runs frames until `done()` is true or three seconds of real time have passed; returns done().
// For what waits on real time: a change the user didn't make (updates arriving) is published at
// most once per ContextPublisher interval of real time, and the Test Engine's fast mode runs many
// frames in that interval.
template <typename Predicate>
bool yield_until(ImGuiTestContext* ctx, Predicate done, std::chrono::seconds limit = 3s) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        ctx->Yield();
    }
    return done();
}

using TestBody = void (*)(ImGuiTestContext*);

// Runs a test's body, failing just that test, and saying why, if an exception escapes it (reading
// a JSON field of the wrong type, say). Uncaught, it would end the whole program, and every test
// after it would go unrun.
void run_body(ImGuiTestContext* ctx) {
    const TestBody body = *static_cast<const TestBody*>(ctx->Test->UserData);
    try {
        body(ctx);
    } catch (const std::exception& error) {
        IM_ERRORF("an exception escaped the test: %s", error.what());
    }
}

// Registers a test that runs the viewer's loop, with `body` as its test function.
ImGuiTest* viewer_test(ImGuiTestEngine* engine, const char* name, TestBody body) {
    // The bodies live here, where their addresses stay fixed, for the tests' UserData to point at.
    static std::deque<TestBody> bodies;
    bodies.push_back(body);
    ImGuiTest* t = IM_REGISTER_TEST(engine, "viewer", name);
    t->SetVarsDataType<UiVars>(set_up);
    t->GuiFunc = viewer_frame;
    t->TestFunc = run_body;
    t->UserData = &bodies.back();
    return t;
}

void register_viewer_tests(ImGuiTestEngine* engine) {
    viewer_test(engine, "waits_for_market_data", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        ctx->Yield();
        IM_CHECK(contains(vars.text, "market data: waiting for the exchange"));
        IM_CHECK(contains(vars.text, "updates received: 0  recoveries: 0"));
        IM_CHECK(contains(vars.text, "ticker 0: no orders"));
        IM_CHECK(!contains(vars.text, "seconds since last update"));
    });

    viewer_test(engine, "shows_the_book_asks_above_bids_each_queue_in_fill_order", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        add_book(vars);
        ctx->SetRef(fillscope::kViewerWindow);
        ctx->Yield(2);
        const char* rows[] = {"**/sell##order10", "**/sell##order11", "**/sell##order12",
                              "**/buy##order1",   "**/buy##order2",   "**/buy##order3"};
        for (std::size_t i = 1; i < std::size(rows); ++i) {
            IM_CHECK_LT(top_of(ctx, rows[i - 1]), top_of(ctx, rows[i]));
        }
        // A level's price is shown once, on its first order's row: below that row's top (a button
        // has its own padding), above the next row's.
        const auto on_row = [&](const char* price, const char* row, const char* next_row) {
            return top_of(ctx, row) <= top_of(ctx, price) && top_of(ctx, price) < top_of(ctx, next_row);
        };
        IM_CHECK(on_row("**/101##levelsell", "**/sell##order11", "**/sell##order12"));
        IM_CHECK(on_row("**/100##levelbuy", "**/buy##order1", "**/buy##order2"));
        IM_CHECK(contains(vars.text, "#2"));  // order 12's place in 101's queue
    });

    viewer_test(engine, "shows_where_the_feed_is", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        add_book(vars);
        ctx->Yield();
        IM_CHECK(contains(vars.text, "market data: live, the books are as of update 6"));
        IM_CHECK(contains(vars.text, "updates received: 6  recoveries: 0"));
        IM_CHECK(contains(vars.text, "seconds since last update: "));
        vars.app->apply_incremental(add(8, 20, Side::buy, 98, 1, 1), std::chrono::steady_clock::now());  // 7 missed
        ctx->Yield();
        IM_CHECK(contains(vars.text, "market data: recovering, the books are as of update 6: update 8 arrived when 7 was "
                                     "next; waiting for a snapshot"));
        vars.app->state().feed_error = "a datagram of 10 bytes isn't a whole number of 42-byte updates";
        ctx->Yield();
        IM_CHECK(contains(vars.text, "last market data error: a datagram of 10 bytes"));
    });

    viewer_test(engine, "the_paint_count_keeps_rising", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        ctx->Yield();
        const auto before = vars.app->paints();
        ctx->Yield(5);
        IM_CHECK_EQ(vars.app->paints(), before + 5);
        IM_CHECK(contains(vars.text, std::format("viewer paints: {} ", before + 4)));
    });

    viewer_test(engine, "the_seconds_since_the_last_update_count_from_it", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        vars.app->apply_incremental(add(1, 1, Side::buy, 100, 1, 1), std::chrono::steady_clock::now() - 5s);
        ctx->Yield();
        const auto at = vars.text.find("seconds since last update: ");
        IM_CHECK(at != std::string::npos);
        const double seconds = std::stod(vars.text.substr(at + std::string_view{"seconds since last update: "}.size()));
        IM_CHECK(seconds >= 5.0 && seconds < 7.0);
    });

    viewer_test(engine, "the_context_bar_starts_with_the_background", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        ctx->Yield();
        IM_CHECK(contains(vars.text, "Context waiting for the exchange's market data · showing ticker 0"));
        add_book(vars);
        // Updates arriving aren't the user's doing, so the bar changes once the publisher's interval
        // has passed and the file says so, not before.
        IM_CHECK(yield_until(ctx, [&] { return contains(vars.text, "Context exchange live at update 6 · showing ticker 0"); }));
        IM_CHECK(!contains(vars.text, "Pointing at"));
        IM_CHECK(!contains(vars.text, "Following"));
        const nlohmann::json context = read_context(vars);
        IM_CHECK(context["point"].is_null());
        IM_CHECK(context["follow"].is_null());
        IM_CHECK_EQ(context["background"]["feed"]["lastSeq"].get<int>(), 6);
    });

    viewer_test(engine, "clicking_an_order_points_at_it", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        add_book(vars);
        ctx->SetRef(fillscope::kViewerWindow);
        ctx->Yield(2);
        ctx->ItemClick("**/buy##order2");
        ctx->Yield(2);

        IM_CHECK((vars.app->selection().pointed() == fillscope::PointTarget{fillscope::OrderRef{0, 2}}));
        IM_CHECK(contains(vars.text, "Pointing at order 2 of ticker 0: buy 7 @ 100, 1 order (5) ahead of it in its queue"));
        const nlohmann::json context = read_context(vars);
        IM_CHECK_EQ(context["point"]["order"]["orderId"].get<int>(), 2);
        IM_CHECK_EQ(context["point"]["order"]["resting"]["ordersAhead"].get<int>(), 1);
        IM_CHECK_EQ(context["point"]["asOfSeq"].get<int>(), 6);
        IM_CHECK(marked(ctx, "**/buy##order2"));
        IM_CHECK(!marked(ctx, "**/buy##order1"));
    });

    viewer_test(engine, "clicking_a_price_points_at_its_level", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        add_book(vars);
        ctx->SetRef(fillscope::kViewerWindow);
        ctx->Yield(2);
        ctx->ItemClick("**/101##levelsell");
        ctx->Yield(2);

        IM_CHECK(contains(vars.text, "Pointing at sell 101 of ticker 0: 6 in 2 orders"));
        const nlohmann::json context = read_context(vars);
        IM_CHECK_EQ(context["point"]["level"]["price"].get<int>(), 101);
        IM_CHECK_EQ(context["point"]["level"]["orders"][0]["orderId"].get<int>(), 11);
        // The row behind the price doesn't take the click: no order is pointed at.
        IM_CHECK(!context["point"].contains("order"));
        IM_CHECK(marked(ctx, "**/101##levelsell"));
        IM_CHECK(!marked(ctx, "**/sell##order11"));
    });

    viewer_test(engine, "clicking_an_id_follows_the_order", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        add_book(vars);
        ctx->SetRef(fillscope::kViewerWindow);
        ctx->Yield(2);
        ctx->ItemClick("**/11##follow");
        ctx->Yield(2);

        IM_CHECK(vars.app->selection().followed() == (fillscope::OrderRef{0, 11}));
        IM_CHECK(contains(vars.text, "Following order 11 of ticker 0: 1 event, sell 2 @ 101, 0 orders (0) ahead of it in its queue"));
        IM_CHECK(contains(vars.text, "order 11 of ticker 0: 1 event"));  // the history's heading
        const nlohmann::json context = read_context(vars);
        IM_CHECK_EQ(context["follow"]["order"]["orderId"].get<int>(), 11);
        IM_CHECK_EQ(context["follow"]["events"][0]["type"].get<std::string>(), "add");
        IM_CHECK(context["point"].is_null());  // the row behind the id doesn't take the click
        IM_CHECK(marked(ctx, "**/11##follow"));
        IM_CHECK(!marked(ctx, "**/12##follow"));
    });

    viewer_test(engine, "a_followed_orders_history_grows_as_it_fills", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        add_book(vars);
        ctx->SetRef(fillscope::kViewerWindow);
        ctx->Yield(2);
        ctx->ItemClick("**/11##follow");
        ctx->Yield(2);
        // A buy takes 1 of order 11's 2, then the rest.
        const auto now = std::chrono::steady_clock::now();
        vars.app->apply_incremental({.seq = 7, .type = UpdateType::trade, .ticker = 0, .side = Side::buy, .price = 101, .qty = 1}, now);
        vars.app->apply_incremental({.seq = 8, .type = UpdateType::modify, .order_id = 11, .ticker = 0, .side = Side::sell, .price = 101, .qty = 1}, now);
        vars.app->apply_incremental({.seq = 9, .type = UpdateType::cancel, .order_id = 11, .ticker = 0, .side = Side::sell, .price = 101}, now);
        IM_CHECK(yield_until(ctx, [&] {
            return contains(vars.text, "Following order 11 of ticker 0: 3 events, not in the book (its last update: cancel at update 9)");
        }));
        IM_CHECK(contains(vars.text, "order 11 of ticker 0: 3 events"));
        IM_CHECK(!ctx->ItemExists("**/sell##order11"));
        IM_CHECK_EQ(read_context(vars)["follow"]["eventCount"].get<int>(), 3);
    });

    viewer_test(engine, "choosing_a_ticker_shows_its_book", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        add_book(vars);
        vars.app->apply_incremental(add(7, 50, Side::sell, 300, 9, 1, 3), std::chrono::steady_clock::now());
        ctx->SetRef(fillscope::kViewerWindow);
        ctx->Yield(2);
        IM_CHECK(marked(ctx, "**/0##ticker"));
        IM_CHECK(ctx->ItemExists("**/sell##order10"));
        IM_CHECK(!ctx->ItemExists("**/sell##order50"));
        ctx->ItemClick("**/3##ticker");
        ctx->Yield(2);

        IM_CHECK_EQ(vars.app->state().shown_ticker, 3u);
        IM_CHECK(ctx->ItemExists("**/sell##order50"));
        IM_CHECK(!ctx->ItemExists("**/sell##order10"));
        IM_CHECK(marked(ctx, "**/3##ticker"));
        IM_CHECK(!marked(ctx, "**/0##ticker"));
        IM_CHECK(contains(vars.text, "showing ticker 3"));
        IM_CHECK_EQ(read_context(vars)["background"]["shownTicker"].get<int>(), 3);
    });

    viewer_test(engine, "the_last_action_has_the_higher_seq", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        add_book(vars);
        ctx->SetRef(fillscope::kViewerWindow);
        ctx->Yield(2);
        ctx->ItemClick("**/buy##order1");
        ctx->ItemClick("**/3##follow");
        ctx->Yield(2);
        nlohmann::json context = read_context(vars);
        IM_CHECK_GT(context["follow"]["seq"].get<int>(), context["point"]["seq"].get<int>());
        ctx->ItemClick("**/99##levelbuy");
        ctx->Yield(2);
        context = read_context(vars);
        IM_CHECK_GT(context["point"]["seq"].get<int>(), context["follow"]["seq"].get<int>());
    });

    viewer_test(engine, "clear_buttons_clear_each_section", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        add_book(vars);
        ctx->SetRef(fillscope::kViewerWindow);
        ctx->Yield(2);
        ctx->ItemClick("**/buy##order2");
        ctx->ItemClick("**/10##follow");
        ctx->Yield(2);
        IM_CHECK(contains(vars.text, "Pointing at"));
        IM_CHECK(contains(vars.text, "Following order 10"));

        ctx->ItemClick(std::format("**/{}", fillscope::kClearPointButton).c_str());
        ctx->Yield(2);
        IM_CHECK(!contains(vars.text, "Pointing at"));
        IM_CHECK(read_context(vars)["point"].is_null());
        IM_CHECK(!read_context(vars)["follow"].is_null());

        ctx->ItemClick(std::format("**/{}", fillscope::kClearFollowButton).c_str());
        ctx->Yield(2);
        IM_CHECK(!contains(vars.text, "Following"));
        IM_CHECK(read_context(vars)["follow"].is_null());
        IM_CHECK(!ctx->ItemExists(std::format("**/{}", fillscope::kHistoryTable).c_str()));
    });

    viewer_test(engine, "the_bar_says_exactly_what_the_file_holds", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        add_book(vars);
        ctx->SetRef(fillscope::kViewerWindow);
        ctx->Yield(2);
        ctx->ItemClick("**/100##levelbuy");
        ctx->ItemClick("**/12##follow");
        ctx->Yield(2);

        // The file is the published context, as JSON...
        const fillscope::ContextPublisher& publisher = vars.app->publisher();
        IM_CHECK(publisher.published().has_value());
        const fillscope::Context& published = *publisher.published();
        const nlohmann::json file = read_context(vars);
        const auto expected =
            fillscope::to_json(published, publisher.writer(), publisher.seq(), file["writtenAtMs"].get<std::int64_t>());
        IM_CHECK(file == nlohmann::json::parse(expected.dump()));
        // ...and every line of the bar is drawn from that same context...
        const fillscope::ContextBarText bar = fillscope::bar_text(published);
        IM_CHECK(contains(vars.text, bar.background));
        IM_CHECK(bar.point && contains(vars.text, *bar.point));
        IM_CHECK(bar.follow && contains(vars.text, *bar.follow));
        // ...as are the book's marks, as drawn.
        IM_CHECK(marked(ctx, "**/100##levelbuy"));
        IM_CHECK(!marked(ctx, "**/99##levelbuy"));
        IM_CHECK(marked(ctx, "**/12##follow"));
        IM_CHECK(!marked(ctx, "**/11##follow"));
    });

    viewer_test(engine, "a_failed_write_shows_in_the_bar_and_the_book", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        vars.app->set_publisher(unwritable());
        add_book(vars);
        ctx->SetRef(fillscope::kViewerWindow);
        ctx->Yield(2);
        ctx->ItemClick("**/buy##order2");
        ctx->Yield(2);

        IM_CHECK(contains(vars.text, "NOT published: can't create"));
        // The selection changed, but nothing reached the file, so neither the bar nor the book may
        // claim it did.
        IM_CHECK(vars.app->selection().pointed().has_value());
        IM_CHECK(!contains(vars.text, "Pointing at"));
        IM_CHECK(!marked(ctx, "**/buy##order2"));
    });

    viewer_test(engine, "hovering_says_a_click_sends", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        add_book(vars);
        ctx->SetRef(fillscope::kViewerWindow);
        ctx->Yield(2);
        ctx->MouseMove("**/1##follow");
        // A tooltip appears once the mouse has been still for a moment. The Test Engine's fast mode
        // skips ordinary sleeps, so this one advances the UI's own clock frame by frame.
        ctx->SleepNoSkip(1.0f, 1.0f / 60.0f);
        IM_CHECK(contains(vars.text, "Follow order 1: every update to it, and sends it to the assistant now"));
    });

    viewer_test(engine, "hovering_while_writes_fail_says_the_click_wont_reach_the_assistant", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        vars.app->set_publisher(unwritable());
        add_book(vars);
        ctx->SetRef(fillscope::kViewerWindow);
        ctx->Yield(2);
        IM_CHECK(contains(vars.text, "NOT published"));
        ctx->MouseMove("**/buy##order1");
        ctx->SleepNoSkip(1.0f, 1.0f / 60.0f);
        IM_CHECK(contains(vars.text, "Point at order 1: but publishing is failing (see the context bar), so the "
                                     "assistant won't receive it"));
    });

    // The viewer reading real multicast streams (test-only groups and ports, none the exchange's): a
    // feed it joins while running recovers from a snapshot, and a datagram it can't decode is reported.
    viewer_test(engine, "reads_both_market_data_streams", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        const fillscope::Stream incremental{"233.252.17.1", 23001}, snapshot{"233.252.17.2", 23002};
        auto joined_incremental = fillscope::FeedSocket::join(incremental);
        auto joined_snapshot = fillscope::FeedSocket::join(snapshot);
        IM_CHECK(joined_incremental.has_value() && joined_snapshot.has_value());
        vars.incremental.emplace(std::move(*joined_incremental));
        vars.snapshot.emplace(std::move(*joined_snapshot));
        const auto send = [](const fillscope::Stream& stream, const MarketUpdate& update) {
            const auto bytes = fillscope::encode_update(update);
            return fillscope::test::send_multicast(stream.group, stream.port, bytes.data(), bytes.size());
        };

        IM_CHECK(send(incremental, add(5, 7, Side::buy, 100, 3, 1)));  // the feed was already running
        IM_CHECK(yield_until(ctx, [&] { return contains(vars.text, "market data: recovering"); }));

        std::vector<MarketUpdate> updates{{.seq = 0, .type = UpdateType::snapshot_start, .order_id = 4}};
        for (std::uint32_t ticker = 0; ticker < fillscope::BookBuilder::kTickers; ++ticker) {
            updates.push_back({.seq = updates.size(), .type = UpdateType::clear, .ticker = ticker});
            if (ticker == 0) {
                MarketUpdate order = add(0, 6, Side::sell, 104, 2, 1);
                order.seq = updates.size();
                updates.push_back(order);
            }
        }
        updates.push_back({.seq = updates.size(), .type = UpdateType::snapshot_end, .order_id = 4});
        std::vector<std::byte> datagram;  // the exchange sends a snapshot's updates several to a datagram
        for (const MarketUpdate& update : updates) {
            const auto bytes = fillscope::encode_update(update);
            datagram.insert(datagram.end(), bytes.begin(), bytes.end());
        }
        IM_CHECK(fillscope::test::send_multicast(snapshot.group, snapshot.port, datagram.data(), datagram.size()));
        IM_CHECK(yield_until(ctx, [&] { return contains(vars.text, "market data: live, the books are as of update 5"); }));
        ctx->SetRef(fillscope::kViewerWindow);
        IM_CHECK(ctx->ItemExists("**/sell##order6"));
        IM_CHECK(ctx->ItemExists("**/buy##order7"));

        IM_CHECK(fillscope::test::send_multicast(incremental.group, incremental.port, datagram.data(), 10));
        IM_CHECK(yield_until(ctx, [&] { return contains(vars.text, "last market data error: a datagram of 10 bytes"); }));
        vars.incremental.reset();
        vars.snapshot.reset();
    });

    viewer_test(engine, "finish_publishes_that_the_viewer_has_stopped", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        add_book(vars);
        ctx->Yield(2);
        IM_CHECK(vars.app->finish());
        const nlohmann::json context = read_context(vars);
        IM_CHECK_EQ(context["writer"]["running"].get<bool>(), false);
        IM_CHECK_EQ(context["background"]["feed"]["lastSeq"].get<int>(), 6);

        vars.app->set_publisher(unwritable());
        IM_CHECK(!vars.app->finish());
    });

    viewer_test(engine, "publishing_failing_and_recovering_is_reported_once_each", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        ctx->Yield(2);
        IM_CHECK(!vars.app->publish_report().has_value());  // working from the start: nothing to say
        const std::filesystem::path file = vars.app->publisher().file();
        vars.app->set_publisher(unwritable());
        add_book(vars);
        ctx->Yield(2);
        const auto failing = vars.app->publish_report();
        IM_CHECK(failing.has_value() && failing->starts_with("can't publish the context to "));
        IM_CHECK(!vars.app->publish_report().has_value());  // said once
        vars.app->set_publisher(file);
        IM_CHECK(yield_until(ctx, [&] { return !vars.app->publisher().error().has_value() && vars.app->publisher().published(); }));
        const auto working = vars.app->publish_report();
        IM_CHECK(working.has_value() && working->starts_with("publishing the context to "));
    });
}

}  // namespace

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : "all";

    ImGuiApp* app = ImGuiApp_ImplNull_Create();
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    app->InitCreateWindow(app, "viewer UI tests", ImVec2(900, 700));
    app->InitBackends(app);

    ImGuiTestEngine* engine = ImGuiTestEngine_CreateContext();
    ImGuiTestEngineIO& test_io = ImGuiTestEngine_GetIO(engine);
    test_io.ConfigRunSpeed = ImGuiTestRunSpeed_Fast;
    test_io.ConfigNoThrottle = true;
    test_io.ConfigLogToTTY = true;
    test_io.ConfigVerboseLevel = ImGuiTestVerboseLevel_Info;
    test_io.ConfigVerboseLevelOnError = ImGuiTestVerboseLevel_Debug;
    test_io.ConfigSavedSettings = false;
    test_io.ConfigCaptureEnabled = false;

    ImGuiTestEngine_Start(engine, ImGui::GetCurrentContext());
    register_viewer_tests(engine);
    ImGuiTestEngine_QueueTests(engine, ImGuiTestGroup_Tests, filter, ImGuiTestRunFlags_RunFromCommandLine);

    while (!ImGuiTestEngine_IsTestQueueEmpty(engine)) {
        if (!app->NewFrame(app)) {
            break;
        }
        ImGui::NewFrame();
        ImGui::Render();
        app->Render(app);
        ImGuiTestEngine_PreSwap(engine);
        app->Present(app);
        ImGuiTestEngine_PostSwap(engine);
    }

    ImGuiTestEngineResultSummary summary;
    ImGuiTestEngine_GetResultSummary(engine, &summary);
    // Each failed test by name, for a reader (and mutants.py) to see which.
    ImVector<ImGuiTest*> tests;
    ImGuiTestEngine_GetTestList(engine, &tests);
    for (const ImGuiTest* test : tests) {
        if (test->Output.Status == ImGuiTestStatus_Error) {
            std::printf("FAILED: %s\n", test->Name);
        }
    }
    ImGuiTestEngine_Stop(engine);
    app->ShutdownBackends(app);
    app->ShutdownCloseWindow(app);
    ImGui::DestroyContext();
    ImGuiTestEngine_DestroyContext(engine);
    app->Destroy(app);

    std::error_code ignored;
    std::filesystem::remove_all(scratch_root(), ignored);
    std::printf("viewer UI tests: %d passed of %d run\n", summary.CountSuccess, summary.CountTested);
    return summary.CountTested > 0 && summary.CountSuccess == summary.CountTested ? 0 : 1;
}
