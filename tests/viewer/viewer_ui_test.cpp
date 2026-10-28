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
//   viewer_ui_tests                  the live viewer's tests
//   viewer_ui_tests --replays        replays', answers' and sessions'
//   viewer_ui_tests NAME             the tests whose names contain NAME, in either part
//
// Exits 0 only if at least one test ran and every test that ran passed.

#include "feed/feed_socket.h"
#include "feed/kept_file.h"
#include "feed/market_update.h"
#include "feed/simulated_exchange.h"
#include "support/answer_fixture.h"
#include "support/session_fixture.h"
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

#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <deque>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
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
    // A clock the test controls, for what depends on time (playing a time shift); real time if unset.
    std::optional<std::chrono::steady_clock::time_point> now;
};

// Runs before the test's first frame: a viewer publishing to this test's own scratch file.
void set_up(ImGuiTestContext* ctx, UiVars& vars) {
    const std::filesystem::path dir = scratch_root() / ctx->Test->Name;
    std::filesystem::remove_all(dir);
    vars.app = std::make_unique<fillscope::ViewerApp>(dir / "context.json");
}

bool contains(const std::string& text, std::string_view part) { return text.find(part) != std::string::npos; }

// Whether the context bar's heading is followed, on the next line, by a background starting with
// `background`: the background belongs under the heading, not beside it.
bool bar_starts(const std::string& text, std::string_view background) {
    return contains(text, std::format("{}\n{}", fillscope::kContextHeading, background));
}

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
    const auto now = vars.now.value_or(std::chrono::steady_clock::now());
    if (vars.incremental && vars.snapshot) {
        vars.app->receive(*vars.incremental, *vars.snapshot, now);
    }
    vars.app->poll_answer(now);
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

// The program's tests are in two parts, each run by its own CTest test, so neither takes more than
// half the time: the live viewer's (viewer_ui_tests, with no arguments), and replays', answers' and
// sessions' (viewer_replay_ui_tests, run with --replays). As one program it took 3.6 s alone in the
// sanitizer build, but over its 10 s beside the tests CTest starts with it (2026-10-27,
// docs/TROUBLESHOOTING.md, case 2). Given a name to filter by, it registers both parts.
enum class Part : std::uint8_t { live, replays, both };
Part g_part = Part::live;

// Registers a test that runs the viewer's loop, with `body` as its test function, in the live part;
// replay_test, in the replays' part.
ImGuiTest* register_test(ImGuiTestEngine* engine, const char* name, TestBody body);
ImGuiTest* viewer_test(ImGuiTestEngine* engine, const char* name, TestBody body) {
    return g_part == Part::replays ? nullptr : register_test(engine, name, std::move(body));
}
ImGuiTest* replay_test(ImGuiTestEngine* engine, const char* name, TestBody body) {
    return g_part == Part::live ? nullptr : register_test(engine, name, std::move(body));
}

ImGuiTest* register_test(ImGuiTestEngine* engine, const char* name, TestBody body) {
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
        IM_CHECK(bar_starts(vars.text, "Waiting for the exchange's market data · showing ticker 0"));
        add_book(vars);
        // Updates arriving aren't the user's doing, so the bar changes once the publisher's interval
        // has passed and the file says so, not before.
        IM_CHECK(yield_until(ctx, [&] { return bar_starts(vars.text, "Exchange live at update 6 · showing ticker 0"); }));
        IM_CHECK(!contains(vars.text, "Pointing at"));
        IM_CHECK(!contains(vars.text, "Following"));
        const nlohmann::json context = read_context(vars);
        IM_CHECK(context["point"].is_null());
        IM_CHECK(context["follow"].is_null());
        IM_CHECK_EQ(context["background"]["feed"]["lastSeq"].get<int>(), 6);
    });

    // Live, the viewer keeps every update it applies (feed/time_shift.h), from before update 1: the
    // books after any of them can be shown, as they were then.
    viewer_test(engine, "live_it_keeps_every_update_it_applies", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        add_book(vars);
        ctx->Yield(2);
        const fillscope::TimeShiftBuffer& kept = vars.app->kept();
        IM_CHECK((kept.runs() == std::vector<fillscope::TimeShiftBuffer::Run>{{.first = 0, .last = 6}}));
        const auto newest = kept.moment(6);
        IM_CHECK_RETV(newest.has_value(), void());
        IM_CHECK(newest->books[0].orders() == vars.app->state().books.book(0).orders());
        const auto third = kept.moment(3);  // asks 10 at 102, 11 and 12 at 101: no bids yet
        IM_CHECK_RETV(third.has_value(), void());
        IM_CHECK_EQ(third->books[0].order_count(), 3u);
        IM_CHECK(third->books[0].levels(Side::buy).empty());
        IM_CHECK(kept.arrived(6).has_value());
    });

    // Joined late, the viewer keeps from the books a snapshot rebuilt, and the updates after.
    viewer_test(engine, "live_after_a_recovery_it_keeps_from_the_rebuilt_books", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        const auto now = std::chrono::steady_clock::now();
        vars.app->apply_incremental(add(4, 1, Side::buy, 100, 5, 1), now);  // updates 1 to 3 were before it joined
        // A snapshot as of update 4: order 1, resting.
        std::vector<MarketUpdate> snapshot{{.seq = 0, .type = UpdateType::snapshot_start, .order_id = 4}};
        for (std::uint32_t ticker = 0; ticker < fillscope::BookBuilder::kTickers; ++ticker) {
            snapshot.push_back({.type = UpdateType::clear, .ticker = ticker});
            if (ticker == 0) {
                snapshot.push_back(add(0, 1, Side::buy, 100, 5, 1));
            }
        }
        snapshot.push_back({.type = UpdateType::snapshot_end, .order_id = 4});
        for (std::uint64_t i = 0; i < snapshot.size(); ++i) {
            snapshot[i].seq = i;
            vars.app->apply_snapshot(snapshot[i]);
        }
        vars.app->apply_incremental(add(5, 2, Side::sell, 101, 3, 1), now);
        ctx->Yield(2);
        const fillscope::TimeShiftBuffer& kept = vars.app->kept();
        IM_CHECK((kept.runs() == std::vector<fillscope::TimeShiftBuffer::Run>{{.first = 4, .last = 5}}));
        IM_CHECK_EQ(kept.moment(4).value().books[0].order_count(), 1u);
        IM_CHECK_EQ(kept.moment(5).value().books[0].order_count(), 2u);
        IM_CHECK(!kept.moment(3).has_value());
    });

    // ---- time-shifting, live (feed/time_shift.h) ----

    // Pause holds the books shown at the newest update while updates keep arriving and are kept; the
    // screen, the bar and the context say which update is shown and how far behind live it is.
    viewer_test(engine, "pause_holds_the_books_while_updates_arrive", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        const auto t0 = std::chrono::steady_clock::now();
        vars.now = t0;
        add_book(vars);  // updates 1 to 6
        ctx->SetRef(fillscope::kViewerWindow);
        ctx->Yield(2);
        IM_CHECK(contains(vars.text, "showing: live, the newest update; 6 updates kept"));
        ctx->ItemClick(std::format("**/{}", fillscope::kShiftPause).c_str());
        ctx->Yield(2);
        // Four more arrive, 100 ms apart: kept, and live, but not shown.
        for (std::uint64_t seq = 7; seq <= 10; ++seq) {
            vars.now = t0 + std::chrono::milliseconds{100 * (seq - 6)};
            vars.app->apply_incremental(add(seq, seq + 10, Side::buy, 90, 1, seq), *vars.now);
        }
        ctx->Yield(2);
        IM_CHECK_EQ(vars.app->state().books.last_seq(), 6u);
        IM_CHECK_EQ(vars.app->state().books.book(0).order_count(), 6u);
        IM_CHECK_EQ(vars.app->state().live.last_seq(), 10u);
        IM_CHECK(!ctx->ItemExists("**/buy##order17"));
        IM_CHECK(contains(vars.text, "showing: update 6, paused, 4 updates (0.4 s) behind live"));
        IM_CHECK(contains(vars.text, "market data: live, the books are as of update 10"));  // the feed's
        IM_CHECK(bar_starts(vars.text, "Exchange live at update 10 · paused at update 6, 4 updates behind (0.4 s) · showing ticker 0"));
        yield_until(ctx, [&] { return read_context(vars)["background"]["timeShift"].is_object(); });
        const nlohmann::json shift = read_context(vars)["background"]["timeShift"];
        IM_CHECK_EQ(shift["shownSeq"].get<int>(), 6);
        IM_CHECK_EQ(shift["newestSeq"].get<int>(), 10);
        IM_CHECK_EQ(shift["behindUpdates"].get<int>(), 4);
        IM_CHECK(!shift["playing"].get<bool>());
        IM_CHECK_EQ(read_context(vars)["background"]["feed"]["lastSeq"].get<int>(), 10);
    });

    // Paused, the controls step back and forward over what is kept, and to the oldest; live returns
    // to the newest, and follows it again.
    viewer_test(engine, "paused_it_steps_over_what_is_kept_and_live_returns", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        add_book(vars);
        ctx->SetRef(fillscope::kViewerWindow);
        ctx->Yield(2);
        ctx->ItemClick(std::format("**/{}", fillscope::kShiftPause).c_str());
        ctx->ItemClick(std::format("**/{}", fillscope::kShiftBack).c_str());
        ctx->Yield(2);
        IM_CHECK_EQ(vars.app->state().books.last_seq(), 5u);
        IM_CHECK(!ctx->ItemExists("**/buy##order3"));  // update 6 added it
        IM_CHECK(ctx->ItemExists("**/buy##order2"));
        ctx->ItemClick(std::format("**/{}", fillscope::kShiftOldest).c_str());
        ctx->Yield(2);
        IM_CHECK_EQ(vars.app->state().books.last_seq(), 0u);
        IM_CHECK(contains(vars.text, "ticker 0: no orders"));
        ctx->ItemClick(std::format("**/{}", fillscope::kShiftForward).c_str());
        ctx->Yield(2);
        IM_CHECK_EQ(vars.app->state().books.last_seq(), 1u);
        IM_CHECK(ctx->ItemExists("**/sell##order10"));
        vars.app->apply_incremental(add(7, 20, Side::buy, 95, 1, 1), std::chrono::steady_clock::now());
        ctx->ItemClick(std::format("**/{}", fillscope::kShiftLive).c_str());
        ctx->Yield(2);
        IM_CHECK(!vars.app->state().time_shift);
        IM_CHECK_EQ(vars.app->state().books.last_seq(), 7u);
        IM_CHECK(ctx->ItemExists("**/buy##order20"));
        IM_CHECK(contains(vars.text, "showing: live, the newest update"));
        IM_CHECK(!contains(vars.text, "paused at update"));
        // Following live again: the next update is shown as it arrives.
        vars.app->apply_incremental(add(8, 21, Side::buy, 94, 1, 1), std::chrono::steady_clock::now());
        ctx->Yield(2);
        IM_CHECK(ctx->ItemExists("**/buy##order21"));
    });

    // Play goes on from the update shown at the pace the updates arrived, so it stays as far behind
    // live as it was when it began (on a clock the test controls).
    viewer_test(engine, "play_follows_the_arrival_pace_and_stays_behind_live", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        const auto t0 = std::chrono::steady_clock::now();
        const auto at = [&](int ms) { return t0 + std::chrono::milliseconds{ms}; };
        // Updates 1 to 5 arrive at 100 ms to 500 ms.
        for (std::uint64_t seq = 1; seq <= 5; ++seq) {
            vars.app->apply_incremental(add(seq, seq, Side::buy, 100 - static_cast<std::int64_t>(seq), 1, 1), at(100 * static_cast<int>(seq)));
        }
        vars.now = at(500);
        ctx->SetRef(fillscope::kViewerWindow);
        ctx->Yield(2);
        ctx->ItemClick(std::format("**/{}", fillscope::kShiftPause).c_str());  // at update 5
        ctx->ItemClick(std::format("**/{}", fillscope::kShiftOldest).c_str());  // to update 0
        ctx->Yield(2);
        // 6 to 10 arrive meanwhile, at 600 ms to 1 s.
        for (std::uint64_t seq = 6; seq <= 10; ++seq) {
            vars.app->apply_incremental(add(seq, seq, Side::buy, 100 - static_cast<std::int64_t>(seq), 1, 1), at(100 * static_cast<int>(seq)));
        }
        // Play from update 0 at 2 s. Update 0 has no arrival time: playing takes update 1's, 100 ms;
        // so at 2.25 s the clock is at 350 ms: updates 1 to 3 have arrived.
        vars.now = at(2000);
        ctx->ItemClick(std::format("**/{}", fillscope::kShiftPlay).c_str());
        vars.now = at(2250);
        ctx->Yield(2);
        IM_CHECK_EQ(vars.app->state().time_shift->shown, 3u);
        IM_CHECK(vars.app->state().time_shift->playing);
        IM_CHECK(contains(vars.text, "showing: update 3, playing, 7 updates (0.7 s) behind live"));
        // At 2.95 s, 1,050 ms: all ten; it plays no further than the newest.
        vars.now = at(2950);
        ctx->Yield(2);
        IM_CHECK_EQ(vars.app->state().time_shift->shown, 10u);
        // Update 11 arrives at 3 s; the play clock is at 1.1 s, so it waits: behind live by the time it
        // began behind.
        vars.app->apply_incremental(add(11, 11, Side::buy, 80, 1, 1), at(3000));
        vars.now = at(3000);
        ctx->Yield(2);
        IM_CHECK_EQ(vars.app->state().time_shift->shown, 10u);
        IM_CHECK(contains(vars.text, "showing: update 10, playing, 1 update ("));
        // Pause stops it where it is.
        ctx->ItemClick(std::format("**/{}", fillscope::kShiftPause).c_str());
        vars.now = at(9000);
        ctx->Yield(2);
        IM_CHECK_EQ(vars.app->state().time_shift->shown, 10u);
        IM_CHECK(!vars.app->state().time_shift->playing);
    });

    // Paused, what is pointed at is the order as of the update shown, wherever the live books have
    // gone: the order cancelled since is still resting where the screen shows it.
    viewer_test(engine, "pointing_while_paused_stays_on_the_order_as_shown", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        add_book(vars);
        ctx->SetRef(fillscope::kViewerWindow);
        ctx->Yield(2);
        ctx->ItemClick(std::format("**/{}", fillscope::kShiftPause).c_str());
        ctx->Yield(2);
        vars.app->apply_incremental({.seq = 7, .type = UpdateType::cancel, .order_id = 2, .ticker = 0, .side = Side::buy, .price = 100,
                                     .qty = 0, .priority = 0},
                                    std::chrono::steady_clock::now());
        ctx->ItemClick("**/buy##order2");
        ctx->Yield(2);
        IM_CHECK(contains(vars.text, "Pointing at order 2 of ticker 0: buy 7 @ 100, 1 order (5) ahead of it in its queue"));
        const nlohmann::json context = read_context(vars);
        IM_CHECK_EQ(context["point"]["asOfSeq"].get<int>(), 6);
        IM_CHECK_EQ(context["point"]["order"]["resting"]["ordersAhead"].get<int>(), 1);
        // Back to live, it has gone: cancelled at update 7.
        ctx->ItemClick(std::format("**/{}", fillscope::kShiftLive).c_str());
        ctx->Yield(2);
        IM_CHECK(contains(vars.text, "Pointing at order 2 of ticker 0: not in the book (its last update: cancel at update 7)"));
    });

    // Before the market data is live there is nothing to hold: pause does nothing.
    viewer_test(engine, "pause_waits_for_the_market_data_to_be_live", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        ctx->SetRef(fillscope::kViewerWindow);
        ctx->Yield(2);
        IM_CHECK(contains(vars.text, "showing: live; nothing kept yet"));
        ctx->ItemClick(std::format("**/{}", fillscope::kShiftPause).c_str());
        ctx->Yield(2);
        IM_CHECK(!vars.app->state().time_shift);
        vars.app->pause();  // asked directly, not through the button, which won't ask: still nothing
        IM_CHECK(!vars.app->state().time_shift);
    });

    // While the feed recovers, the books shown are still the last update kept before the gap: pause
    // holds them there, and the moment can be stepped from like any other.
    viewer_test(engine, "pause_while_recovering_holds_the_last_update_before_the_gap", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        const auto now = std::chrono::steady_clock::now();
        for (std::uint64_t seq = 1; seq <= 3; ++seq) {
            vars.app->apply_incremental(add(seq, seq, Side::buy, 100, 1, seq), now);
        }
        vars.app->apply_incremental(add(5, 5, Side::buy, 100, 1, 5), now);  // update 4 lost: recovering
        ctx->SetRef(fillscope::kViewerWindow);
        ctx->Yield(2);
        IM_CHECK(vars.app->state().live.status() == fillscope::BookBuilder::Status::recovering);
        ctx->ItemClick(std::format("**/{}", fillscope::kShiftPause).c_str());
        ctx->Yield(2);
        IM_CHECK(vars.app->state().time_shift.has_value());
        IM_CHECK_EQ(vars.app->state().books.last_seq(), 3u);
        IM_CHECK(contains(vars.text, "showing: update 3, paused, 0 updates"));
        ctx->ItemClick(std::format("**/{}", fillscope::kShiftBack).c_str());
        ctx->Yield(2);
        IM_CHECK_EQ(vars.app->state().books.last_seq(), 2u);
        IM_CHECK(!ctx->ItemExists("**/buy##order3"));
    });

    // Back from the start of a run goes across the gap before it, to the last update kept before it.
    viewer_test(engine, "back_across_a_gap_goes_to_the_last_update_before_it", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        const auto now = std::chrono::steady_clock::now();
        for (std::uint64_t seq = 1; seq <= 3; ++seq) {
            vars.app->apply_incremental(add(seq, seq, Side::buy, 100, 1, seq), now);
        }
        vars.app->apply_incremental(add(5, 5, Side::buy, 100, 1, 5), now);  // update 4 lost
        std::vector<MarketUpdate> snapshot{{.seq = 0, .type = UpdateType::snapshot_start, .order_id = 5}};
        for (std::uint32_t ticker = 0; ticker < fillscope::BookBuilder::kTickers; ++ticker) {
            snapshot.push_back({.type = UpdateType::clear, .ticker = ticker});
        }
        snapshot.push_back(add(0, 5, Side::buy, 100, 1, 5));
        snapshot.push_back({.type = UpdateType::snapshot_end, .order_id = 5});
        for (std::uint64_t i = 0; i < snapshot.size(); ++i) {
            snapshot[i].seq = i;
            vars.app->apply_snapshot(snapshot[i]);
        }
        ctx->SetRef(fillscope::kViewerWindow);
        ctx->Yield(2);
        ctx->ItemClick(std::format("**/{}", fillscope::kShiftPause).c_str());  // at update 5, a run's start
        ctx->ItemClick(std::format("**/{}", fillscope::kShiftBack).c_str());
        ctx->Yield(2);
        IM_CHECK_EQ(vars.app->state().books.last_seq(), 3u);
        IM_CHECK(ctx->ItemExists("**/buy##order3"));
        ctx->ItemClick(std::format("**/{}", fillscope::kShiftForward).c_str());
        ctx->Yield(2);
        IM_CHECK_EQ(vars.app->state().books.last_seq(), 5u);
    });

    // ---- a live moment's address (feed/kept_file.h) ----

    // Live, what is kept is written to a file, and what is pointed at has an address there: the file and
    // the update the books shown are as of, whose books the file rebuilds exactly. The last context,
    // as the viewer exits and deletes the file, gives none.
    viewer_test(engine, "live_a_point_has_an_address_in_the_file_of_what_is_kept", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        const std::filesystem::path file = scratch_root() / ctx->Test->Name / "kept" / "viewer.kept";
        std::filesystem::create_directories(file.parent_path());
        auto writer = fillscope::KeptFileWriter::create(file);
        IM_CHECK_RETV(writer.has_value(), void());
        vars.app->keep_to(std::move(*writer));
        add_book(vars);
        ctx->SetRef(fillscope::kViewerWindow);
        ctx->Yield(2);
        ctx->ItemClick("**/buy##order2");
        ctx->Yield(2);
        nlohmann::json context = read_context(vars);
        IM_CHECK_EQ(context["background"]["keptFile"].get<std::string>(), file.string());
        IM_CHECK_EQ(context["point"]["keptAddress"]["file"].get<std::string>(), file.string());
        IM_CHECK_EQ(context["point"]["keptAddress"]["seq"].get<int>(), 6);
        const auto read = fillscope::read_kept_file(file);
        IM_CHECK_RETV(read.has_value(), void());
        IM_CHECK(read->moment(6).value().books[0].orders() == vars.app->state().books.book(0).orders());
        // Paused and stepped back, the address is the update shown.
        ctx->ItemClick(std::format("**/{}", fillscope::kShiftPause).c_str());
        ctx->ItemClick(std::format("**/{}", fillscope::kShiftBack).c_str());
        ctx->ItemClick("**/buy##order2");
        ctx->Yield(2);
        context = read_context(vars);
        IM_CHECK_EQ(context["point"]["keptAddress"]["seq"].get<int>(), 5);
        IM_CHECK(fillscope::read_kept_file(file).value().moment(5).value().books[0].orders() == vars.app->state().books.book(0).orders());
        IM_CHECK(vars.app->finish());
        context = read_context(vars);
        IM_CHECK(context["background"]["keptFile"].is_null());
        IM_CHECK(context["point"]["keptAddress"].is_null());
    });

    // A write that fails stops the writing, and the address with it, and says so once.
    viewer_test(engine, "a_failed_write_of_what_is_kept_stops_it_and_the_address", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        const std::filesystem::path file = scratch_root() / ctx->Test->Name / "kept" / "viewer.kept";
        std::filesystem::create_directories(file.parent_path());
        vars.app->keep_to(fillscope::KeptFileWriter::create(file).value());
        // Past the largest file the process may write, a write fails rather than ending the process.
        const auto old_handler = std::signal(SIGXFSZ, SIG_IGN);
        rlimit old_limit{};
        ::getrlimit(RLIMIT_FSIZE, &old_limit);
        rlimit limit = old_limit;
        limit.rlim_cur = 2048;
        ::setrlimit(RLIMIT_FSIZE, &limit);
        std::optional<std::string> report;
        for (std::uint64_t seq = 1; seq <= 100 && !report; ++seq) {
            vars.app->apply_incremental(add(seq, seq, Side::buy, 100, 1, seq), std::chrono::steady_clock::now());
            report = vars.app->kept_file_report();
        }
        ::setrlimit(RLIMIT_FSIZE, &old_limit);
        std::signal(SIGXFSZ, old_handler);
        IM_CHECK_RETV(report.has_value(), void());
        IM_CHECK(contains(*report, "writing what is kept stopped: can't write"));
        IM_CHECK(contains(*report, "live moments have no address now"));
        IM_CHECK(!vars.app->kept_file_report());  // once
        IM_CHECK(!vars.app->state().kept_file);
        ctx->Yield(2);
        IM_CHECK(read_context(vars)["background"]["keptFile"].is_null());
    });

    viewer_test(engine, "clicking_an_order_points_at_it", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        add_book(vars);
        ctx->SetRef(fillscope::kViewerWindow);
        ctx->Yield(2);
        const float book_top = top_of(ctx, "**/sell##order10");
        ctx->ItemClick("**/buy##order2");
        ctx->Yield(2);

        IM_CHECK((vars.app->selection().pointed() == fillscope::PointTarget{fillscope::OrderRef{0, 2}}));
        // The bar keeps room for the pointing line, so pointing doesn't move the book, nor does
        // stopping (below).
        IM_CHECK_EQ(top_of(ctx, "**/sell##order10"), book_top);
        IM_CHECK(contains(vars.text, "Pointing at order 2 of ticker 0: buy 7 @ 100, 1 order (5) ahead of it in its queue"));
        const nlohmann::json context = read_context(vars);
        IM_CHECK_EQ(context["point"]["order"]["orderId"].get<int>(), 2);
        IM_CHECK_EQ(context["point"]["order"]["resting"]["ordersAhead"].get<int>(), 1);
        IM_CHECK_EQ(context["point"]["asOfSeq"].get<int>(), 6);
        IM_CHECK(marked(ctx, "**/buy##order2"));
        IM_CHECK(!marked(ctx, "**/buy##order1"));

        ctx->ItemClick(std::format("**/{}", fillscope::kClearPointButton).c_str());
        ctx->Yield(2);
        IM_CHECK(!contains(vars.text, "Pointing at"));
        IM_CHECK_EQ(top_of(ctx, "**/sell##order10"), book_top);
    });

    // The x ending the pointing line, and the following line's, stay inside the window at every
    // width: each line once wrapped at the window's edge with its x after it, so a line that just
    // fit put the x past the edge, where it couldn't be seen.
    viewer_test(engine, "the_bars_x_stays_inside_the_window_at_every_width", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        add_book(vars);
        ctx->SetRef(fillscope::kViewerWindow);
        ctx->Yield(2);
        ctx->ItemClick("**/buy##order2");
        ctx->ItemClick("**/2##follow");
        ctx->Yield(2);
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        for (const char* label : {fillscope::kClearPointButton, fillscope::kClearFollowButton}) {
            const std::string x = std::format("**/{}", label);
            ImGui::GetIO().DisplaySize = size;
            ctx->Yield(2);
            // From well past the line's end to well short of it, a pixel at a time.
            const float line_end = ctx->ItemInfo(x.c_str()).RectFull.Min.x;
            for (float width = line_end + 60.0f; width > line_end - 60.0f; width -= 1.0f) {
                ImGui::GetIO().DisplaySize = ImVec2{width, size.y};
                ctx->Yield(2);
                const ImGuiWindow* window = ctx->GetWindowByRef(std::format("//{}", fillscope::kViewerWindow).c_str());
                const ImRect button = ctx->ItemInfo(x.c_str()).RectFull;
                // Inside the area the window's content is laid out in, short of its padding.
                IM_CHECK_LE(button.Max.x, window->WorkRect.Max.x);
                if (button.Max.x > window->WorkRect.Max.x) {
                    break;  // the first width is enough to say what's wrong
                }
            }
        }
        ImGui::GetIO().DisplaySize = size;
        ctx->Yield(2);
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
        const fillscope::Stream incremental{"233.252.17.1", fillscope::test::test_port(0)},
            snapshot{"233.252.17.2", fillscope::test::test_port(1)};
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

    // An answer (viewer --answer): the pane beside the market, stepped a frame at a time, the books
    // moving with it, a step's question shown before the step is played, and the context naming the
    // step. The answer file appears after the viewer starts, as when the assistant writes one.
    replay_test(engine, "an_answer_is_stepped_through", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        const std::filesystem::path answer = scratch_root() / ctx->Test->Name / ".fillscope" / "answer.json";
        vars.app->watch_answer(answer);
        ctx->SetRef(fillscope::kViewerWindow);
        ctx->Yield(2);
        IM_CHECK(contains(vars.text, "Waiting for an answer at"));
        IM_CHECK(contains(vars.text, "market data: none until an answer is shown"));
        IM_CHECK(yield_until(ctx, [&] { return bar_starts(vars.text, "No answer shown: waiting for an answer at"); }));
        fillscope::test::write_maker_and_taker(answer, "What happens when a taker trades with a maker?");
        IM_CHECK(yield_until(ctx, [&] { return contains(vars.text, "Press > to begin."); }));
        IM_CHECK(contains(vars.text, "introduction; 3 steps, 1 asking a question first, so 4 presses to the end"));
        IM_CHECK(contains(vars.text, "What happens when a taker trades with a maker?"));
        IM_CHECK(contains(vars.text, "A maker posts orders that wait"));
        IM_CHECK(contains(vars.text, "ticker 0: no orders"));
        // In an answer, only its own controls move the books.
        IM_CHECK(!ctx->ItemExists(std::format("**/{}", fillscope::kReplayForward).c_str()));
        IM_CHECK(yield_until(ctx, [&] { return bar_starts(vars.text, "Answer to \"What happens when a taker trades with a maker?\": its introduction"); }));

        const std::string forward = std::format("**/{}", fillscope::kAnswerForward);
        ctx->ItemClick(forward.c_str());
        ctx->Yield(2);
        IM_CHECK(contains(vars.text, "Step 1. The maker sends order 10: buy 10 at 99."));
        IM_CHECK(contains(vars.text, "step 1 of 3\n"));  // no question, so no half named
        IM_CHECK(contains(vars.text, "The maker bids 10 at 99."));
        IM_CHECK(contains(vars.text, "the maker: order 10 accepted: buy 10 at 99"));
        IM_CHECK(contains(vars.text, "The best bid is 99, for 10."));
        IM_CHECK(contains(vars.text, "the maker's 10"));  // the book says whose each order is
        // In an answer, the market data's line says only where the books are: the feed's and the
        // viewer's health are for watching the exchange.
        IM_CHECK(contains(vars.text, "the books as of update 1"));
        IM_CHECK(!contains(vars.text, "a replay"));
        IM_CHECK(!contains(vars.text, "updates received"));
        IM_CHECK(!contains(vars.text, "viewer paints"));

        ctx->ItemClick(forward.c_str());
        ctx->ItemClick(forward.c_str());
        ctx->Yield(2);
        // Step 3's question, the books still as before it: the maker's offer whole.
        IM_CHECK(contains(vars.text, "step 3 of 3: the question"));
        IM_CHECK(contains(vars.text, "The taker buys 4 at 101: what trades?"));
        IM_CHECK(contains(vars.text, "The book before this step:"));
        IM_CHECK(contains(vars.text, "best ask 101 for 10 (first in line: the maker's 11)"));
        IM_CHECK(contains(vars.text, "Press > to see what happens."));
        IM_CHECK(!contains(vars.text, "It trades 4 at 101"));  // the explanation waits for the step
        IM_CHECK(contains(vars.text, "the books as of update 2"));
        IM_CHECK(read_context(vars)["background"]["answer"]["asking"] == true);
        IM_CHECK(read_context(vars)["background"]["answer"]["step"] == 3);

        ctx->ItemClick(forward.c_str());
        ctx->Yield(2);
        IM_CHECK(contains(vars.text, "It trades 4 at 101, the resting order's price."));
        IM_CHECK(contains(vars.text, "step 3 of 3: what happened"));
        IM_CHECK(contains(vars.text, "Market data (public: every trader sees this):"));
        IM_CHECK(contains(vars.text, "update 3: a trade, 4 at 101, caused by an incoming buy"));
        IM_CHECK(contains(vars.text, "update 4: the exchange's order 2 (the maker's 11) now has 6: it was partly filled"));
        IM_CHECK(contains(vars.text, "The exchange tells (privately: each client only its own):"));
        IM_CHECK(contains(vars.text, "best ask 101 for 6 (first in line: the maker's 11)"));
        IM_CHECK(contains(vars.text, "the maker: order 11 filled 4 at 101, 6 left"));
        IM_CHECK(contains(vars.text, "This step trades 4 at 101 (an incoming buy crossing)."));
        IM_CHECK(!contains(vars.text, "FALSE"));
        IM_CHECK(contains(vars.text, "the books as of update 4"));
        IM_CHECK(read_context(vars)["background"]["answer"]["asking"] == false);

        ctx->ItemClick(std::format("**/{}", fillscope::kAnswerBack).c_str());
        ctx->Yield(2);
        IM_CHECK(contains(vars.text, "step 3 of 3: the question"));
        IM_CHECK(contains(vars.text, "the books as of update 2"));
        ctx->ItemClick(std::format("**/{}", fillscope::kAnswerStart).c_str());
        ctx->Yield(2);
        IM_CHECK(contains(vars.text, "Press > to begin."));
        IM_CHECK(contains(vars.text, "ticker 0: no orders"));
        ctx->ItemClick(std::format("**/{}", fillscope::kAnswerEnd).c_str());
        ctx->Yield(2);
        IM_CHECK(contains(vars.text, "It trades 4 at 101"));
    });

    // A new answer replaces the one shown, from its start; one that can't be shown says why and
    // leaves nothing of the last on screen; a claim the viewer finds false is marked so.
    replay_test(engine, "a_new_answer_replaces_the_shown_one", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        const std::filesystem::path answer = scratch_root() / ctx->Test->Name / ".fillscope" / "answer.json";
        fillscope::test::write_maker_and_taker(answer, "First question?");
        vars.app->watch_answer(answer);
        ctx->SetRef(fillscope::kViewerWindow);
        IM_CHECK(yield_until(ctx, [&] { return contains(vars.text, "First question?"); }));
        ctx->ItemClick(std::format("**/{}", fillscope::kAnswerEnd).c_str());
        ctx->Yield(2);
        IM_CHECK(contains(vars.text, "the books as of update 4"));

        fillscope::test::write_maker_and_taker(answer, "Second question?", /*false_claim=*/true);
        // The pane at once; the bar once the context file says so (a change the user didn't make is
        // published at most once per interval), and then nothing of the first answer anywhere.
        IM_CHECK(yield_until(ctx, [&] { return contains(vars.text, "Second question?"); }));
        IM_CHECK(yield_until(ctx, [&] { return !contains(vars.text, "First question?"); }));
        IM_CHECK(contains(vars.text, "Press > to begin."));  // a new answer opens at its start
        ctx->ItemClick(std::format("**/{}", fillscope::kAnswerEnd).c_str());
        ctx->Yield(2);
        IM_CHECK(contains(vars.text, "FALSE"));
        IM_CHECK(contains(vars.text, "In fact, the best ask is 101, for 6."));

        // The replay's market data changed under the answer: stale, so nothing of it is shown. The
        // same answer is then written again, as a new file over the old, so the viewer reads it.
        std::ofstream{answer.parent_path() / "answer" / "market.dat", std::ios::app} << "x";
        std::filesystem::copy_file(answer, answer.string() + ".tmp");
        std::filesystem::rename(answer.string() + ".tmp", answer);
        IM_CHECK(yield_until(ctx, [&] { return contains(vars.text, "Can't show the answer: the answer is stale"); }));
        IM_CHECK(yield_until(ctx, [&] { return !contains(vars.text, "Second question?"); }));
        IM_CHECK(yield_until(ctx, [&] { return bar_starts(vars.text, "No answer shown: the answer at"); }));
        IM_CHECK(contains(vars.text, "market data: none until an answer is shown"));
        IM_CHECK(contains(vars.text, "ticker 0: no orders"));
    });

    // A course (viewer --course): the pane's drop-down offers its lessons beside your latest
    // question. Nothing is chosen at first, so no answer is shown, though the questions' file holds
    // one. Choosing a lesson shows it from its start, under its title rather than its question, and
    // the context names the lesson; choosing "Your latest question" shows the answer file watched.
    replay_test(engine, "a_lesson_is_chosen_from_the_course", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        const std::filesystem::path dir = scratch_root() / ctx->Test->Name;
        const std::filesystem::path built = dir / "course";
        std::filesystem::create_directories(built);
        std::ofstream{built / "course.json"} << R"({"title": "A course", "lessons": [
            {"title": "The first lesson", "scenario": "01-first.json", "read": "a chapter"},
            {"title": "The second lesson", "scenario": "02-second.json"},
            {"title": "A third lesson, whose title is far too long to fit in the drop-down at the top of the answer pane",
             "scenario": "03-third.json"}]})";
        auto course = fillscope::read_course_file(built / "course.json");
        IM_CHECK(course.has_value());
        fillscope::test::write_maker_and_taker(fillscope::lesson_answer(built, course->lessons[0]), "Lesson one?");
        fillscope::test::write_maker_and_taker(fillscope::lesson_answer(built, course->lessons[1]), "Lesson two?");
        fillscope::test::write_maker_and_taker(fillscope::lesson_answer(built, course->lessons[2]), "Lesson three?");
        const std::filesystem::path questions = dir / ".fillscope" / "answer.json";
        fillscope::test::write_maker_and_taker(questions, "My own question?");
        vars.app->watch_answer(questions);
        vars.app->open_course(std::move(*course), built);
        ctx->SetRef(fillscope::kViewerWindow);
        IM_CHECK(yield_until(ctx, [&] { return bar_starts(vars.text, "No answer shown: nothing chosen yet"); }));
        IM_CHECK(contains(vars.text, "Choose a lesson, or your latest question"));
        IM_CHECK(!contains(vars.text, "My own question?"));
        IM_CHECK(contains(vars.text, "ticker 0: no orders"));
        IM_CHECK(read_context(vars)["background"]["answer"].is_null());

        ctx->ItemClick(std::format("**/{}", fillscope::kLessonChoice).c_str());
        ctx->ItemClick("//$FOCUSED/2. The second lesson##choice2");
        ctx->Yield(2);
        IM_CHECK(contains(vars.text, "2. The second lesson"));
        IM_CHECK(!contains(vars.text, "Lesson two?"));  // the title names it, above
        IM_CHECK(contains(vars.text, "Press > to begin."));
        IM_CHECK(read_context(vars)["background"]["answer"]["question"] == "Lesson two?");
        IM_CHECK(read_context(vars)["background"]["answer"]["lesson"] == "lesson 2, \"The second lesson\"");
        IM_CHECK(bar_starts(vars.text, "Lesson 2, \"The second lesson\": its introduction"));

        ctx->ItemClick(std::format("**/{}", fillscope::kLessonChoice).c_str());
        ctx->ItemClick("//$FOCUSED/1. The first lesson##choice1");
        ctx->Yield(2);
        IM_CHECK(read_context(vars)["background"]["answer"]["question"] == "Lesson one?");
        IM_CHECK(contains(vars.text, "Read: a chapter"));

        // A title too long for the drop-down ends in "..." where it is cut, not mid-letter at the
        // arrow. (The bar still gives it whole.)
        ctx->ItemClick(std::format("**/{}", fillscope::kLessonChoice).c_str());
        ctx->ItemClick("//$FOCUSED/3. A third lesson, whose title is far too long to fit in the drop-down at the top of the "
                       "answer pane##choice3");
        ctx->Yield(2);
        IM_CHECK(read_context(vars)["background"]["answer"]["question"] == "Lesson three?");
        IM_CHECK(contains(vars.text, "3. A third lesson"));
        IM_CHECK(contains(vars.text, "..."));
        IM_CHECK(!contains(vars.text, "3. A third lesson, whose title is far too long to fit in the drop-down at the top of the answer pane"));

        ctx->ItemClick(std::format("**/{}", fillscope::kLessonChoice).c_str());
        ctx->ItemClick("//$FOCUSED/Your latest question##choice0");
        ctx->Yield(2);
        IM_CHECK(contains(vars.text, "My own question?"));
        IM_CHECK(read_context(vars)["background"]["answer"]["lesson"].is_null());
    });

    // A line longer than the window wraps rather than widening it: a wider window scrolls sideways,
    // and a click can then leave the book out of reach. A replay's file is named by its absolute
    // path, as long as the directory it is in, and is named twice: in the bar and above the replay's
    // controls.
    replay_test(engine, "a_long_file_name_wraps_rather_than_widening_the_window", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        const std::string deep = std::string(300, 'd') + "/a.dat";
        vars.app->open_replay(deep, fillscope::MarketDataFile{.recording = std::string(300, 'r'), .updates = {add(1, 10, Side::sell, 102, 3, 1)}});
        ctx->SetRef(fillscope::kViewerWindow);
        ctx->Yield(2);
        ctx->ItemClick(std::format("**/{}", fillscope::kReplayEnd).c_str());
        ctx->Yield(2);
        const ImGuiWindow* window = ctx->GetWindowByRef(std::format("//{}", fillscope::kViewerWindow).c_str());
        IM_CHECK(window != nullptr);
        IM_CHECK_EQ(window->ScrollMax.x, 0.0f);
        ctx->ItemClick("**/sell##order10");
        ctx->Yield(2);
        IM_CHECK(contains(vars.text, "Pointing at order 10 of ticker 0"));
    });

    replay_test(engine, "a_replay_goes_to_any_update", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        // add_book's six updates, then order 11 partly filled and order 12 cancelled.
        fillscope::MarketDataFile replay{
            .recording = "runs/a.rec",
            .updates = {add(1, 10, Side::sell, 102, 3, 1), add(2, 11, Side::sell, 101, 2, 1), add(3, 12, Side::sell, 101, 4, 2),
                        add(4, 1, Side::buy, 100, 5, 1), add(5, 2, Side::buy, 100, 7, 2), add(6, 3, Side::buy, 99, 4, 1),
                        {.seq = 7, .type = UpdateType::modify, .order_id = 11, .ticker = 0, .side = Side::sell, .price = 101, .qty = 1},
                        {.seq = 8, .type = UpdateType::cancel, .order_id = 12, .ticker = 0, .side = Side::sell, .price = 101}}};
        vars.app->open_replay("runs/a.dat", std::move(replay));
        ctx->SetRef(fillscope::kViewerWindow);
        ctx->Yield(2);
        IM_CHECK(contains(vars.text, "(recorded in runs/a.rec): after update 0 of 8"));
        IM_CHECK(contains(vars.text, "ticker 0: no orders"));

        ctx->ItemClick(std::format("**/{}", fillscope::kReplayEnd).c_str());
        ctx->Yield(2);
        IM_CHECK(contains(vars.text, "after update 8 of 8"));
        IM_CHECK(!ctx->ItemExists("**/sell##order12"));  // cancelled by update 8
        ctx->ItemClick(std::format("**/{}", fillscope::kReplayBack).c_str());
        ctx->Yield(2);
        IM_CHECK(contains(vars.text, "after update 7 of 8"));
        IM_CHECK(ctx->ItemExists("**/sell##order12"));  // back: before its cancel
        ctx->ItemClick(std::format("**/{}", fillscope::kReplayStart).c_str());
        ctx->ItemClick(std::format("**/{}", fillscope::kReplayForward).c_str());
        ctx->Yield(2);
        IM_CHECK(contains(vars.text, "after update 1 of 8"));
        IM_CHECK(ctx->ItemExists("**/sell##order10"));
        IM_CHECK(!ctx->ItemExists("**/sell##order11"));

        // The context's replay address follows the books: pointed at after update 1, then at the end.
        ctx->ItemClick("**/sell##order10");
        ctx->Yield(2);
        IM_CHECK_EQ(read_context(vars)["point"]["replayAddress"]["seq"].get<int>(), 1);
        IM_CHECK(read_context(vars)["point"]["replayAddress"]["marketDataFile"].get<std::string>().ends_with("/runs/a.dat"));
        IM_CHECK(bar_starts(vars.text, "Replay of "));
        ctx->ItemInputValue(std::format("**/{}", fillscope::kReplaySlider).c_str(), 7);
        ctx->Yield(2);
        IM_CHECK(contains(vars.text, "after update 7 of 8"));
        IM_CHECK_EQ(read_context(vars)["point"]["replayAddress"]["seq"].get<int>(), 7);
        IM_CHECK_EQ(read_context(vars)["background"]["replay"]["updates"].get<int>(), 8);
    });

    // A replay whose update 2 can't be applied (an order it never added): the books stay after update
    // 1, and the status and the bar say so; going forward again doesn't feed the update again.
    replay_test(engine, "a_replay_that_cant_be_followed_says_where_it_stopped", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        fillscope::MarketDataFile replay{
            .recording = "runs/b.rec",
            .updates = {add(1, 10, Side::sell, 102, 3, 1),
                        {.seq = 2, .type = UpdateType::cancel, .order_id = 99, .ticker = 0, .side = Side::sell, .price = 101},
                        add(3, 11, Side::sell, 101, 2, 1)}};
        vars.app->open_replay("runs/b.dat", std::move(replay));
        ctx->SetRef(fillscope::kViewerWindow);
        ctx->Yield(2);
        IM_CHECK(contains(vars.text, "market data: a replay, the books are as of update 0"));
        IM_CHECK(!contains(vars.text, "market data: waiting for the exchange"));
        ctx->ItemClick(std::format("**/{}", fillscope::kReplayEnd).c_str());
        ctx->Yield(2);
        IM_CHECK(contains(vars.text, "market data: a replay, stuck after update 1: "));
        IM_CHECK(contains(vars.text, "updates received: 2 "));
        ctx->ItemClick(std::format("**/{}", fillscope::kReplayForward).c_str());
        ctx->Yield(2);
        IM_CHECK(contains(vars.text, "updates received: 2 "));
        IM_CHECK(contains(vars.text, "at update 1 of 3, stuck: "));
    });

    // A recorded session's replay, stepped update by update: its trades panel draws each trade on the
    // shown ticker up to the update shown, and the slicer's as the truth says (support/session_fixture.h
    // gives it by hand: on ticker 1, update 4 the slicer took, 7 its resting order was met, 9 it
    // wasn't in; ticker 2's trade at 12 is never drawn there). The book names each order's client.
    replay_test(engine, "a_sessions_trades_are_drawn_from_its_truth_update_by_update", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        ctx->SetRef(fillscope::kViewerWindow);
        ctx->Yield(2);
        IM_CHECK(contains(vars.text, "Trades are drawn in a replay. Live, there is no truth to say who traded"));
        fillscope::Session session = fillscope::test::hand_session();
        vars.app->open_session("/sessions/hand/market.dat", std::move(session.market_data), std::move(session.truth));
        ctx->ItemClick("**/1##ticker");
        const std::vector<std::pair<std::uint64_t, std::string>> trades{
            {4, "4: 10 at 105, took"}, {7, "7: 5 at 105, met"}, {9, "9: 4 at 106, none"}};
        std::optional<float> trade4_x;  // where update 4's dot is: the axis stays put, so it does too
        for (std::uint64_t at = 0; at <= 13; ++at) {
            ctx->Yield(2);
            if (at >= 4) {
                const float x = ctx->ItemInfo("**/##trade4").RectFull.Min.x;
                if (!trade4_x) {
                    trade4_x = x;
                }
                IM_CHECK_EQ(x, *trade4_x);
            }
            IM_CHECK(contains(vars.text, std::format("after update {} of 13", at)));
            // How many dots, from the panel's heading (so no frames are spent searching for the dots
            // that shouldn't be there: those still to trade, and ticker 2's), then each one's report.
            std::size_t drawn = 0;
            for (const auto& [update, report] : trades) {
                if (update <= at) {
                    ++drawn;
                    IM_CHECK_EQ(std::string{ctx->ItemInfo(std::format("**/##trade{}", update).c_str()).DebugLabel}, report);
                }
            }
            IM_CHECK(contains(vars.text, at == 0 ? std::string{"Trades on ticker 1: none yet"}
                                                 : std::format("Trades on ticker 1, updates 1 to {}: {}.", at, drawn)));
            ctx->ItemClick(std::format("**/{}", fillscope::kReplayForward).c_str());
        }
        IM_CHECK(!ctx->ItemExists("**/##trade12"));  // ticker 2's, once: the counts above say the rest
        IM_CHECK(contains(vars.text, "Here the slicer buys 15 in pieces of 10"));
        IM_CHECK(contains(vars.text, "Orange: the slicer's"));
        // The book's orders, by client: after update 6, the maker's is gone and the slicer's rests.
        vars.app->replay_to(6);
        ctx->Yield(2);
        IM_CHECK(contains(vars.text, "6 (slicer)"));
        IM_CHECK(contains(vars.text, "5 (random)"));
        IM_CHECK(!contains(vars.text, "1 (maker)"));
        vars.app->replay_to(3);
        ctx->Yield(2);
        IM_CHECK(contains(vars.text, "1 (maker)"));
        IM_CHECK(!contains(vars.text, "6 (slicer)"));
        // A dot goes to its update; ticker 2's slicer trade is drawn on ticker 2's panel.
        vars.app->replay_to(13);
        ctx->Yield(2);
        ctx->ItemClick("**/##trade7");
        ctx->Yield(2);
        IM_CHECK(contains(vars.text, "after update 7 of 13"));
        ctx->ItemClick("**/2##ticker");
        vars.app->replay_to(13);
        ctx->Yield(2);
        IM_CHECK_EQ(std::string{ctx->ItemInfo("**/##trade12").DebugLabel}, std::string{"12: 3 at 50, took"});
        IM_CHECK(contains(vars.text, "The slicer has no order on this ticker."));
    });

    // A replay plays on by itself at the rate chosen, on the test's own clock: paused, it stays; a move
    // of the user's own pauses it; at the end it stops, and play starts it again from the start; a new
    // rate goes on from where it is. And the jumps go to the nearest trade either way, and with a
    // session's truth, the nearest of the slicer's (support/session_fixture.h: on ticker 1, trades at
    // 4, the slicer's, 7, the slicer's, and 9).
    replay_test(engine, "a_replay_plays_by_itself_and_jumps_to_trades", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        const auto t0 = std::chrono::steady_clock::time_point{} + std::chrono::hours{1};
        const auto at = [&](int ms) { return t0 + std::chrono::milliseconds{ms}; };
        vars.now = t0;
        fillscope::Session session = fillscope::test::hand_session();
        vars.app->open_session("/sessions/hand/market.dat", std::move(session.market_data), std::move(session.truth));
        vars.app->show_ticker(1);
        ctx->SetRef(fillscope::kViewerWindow);
        ctx->Yield(2);
        const auto click = [&](const char* item) { ctx->ItemClick(std::format("**/{}", item).c_str()); };
        IM_CHECK(marked(ctx, "**/###replay_rate100"));
        click("###replay_rate10");
        IM_CHECK(marked(ctx, "**/###replay_rate10"));
        click(fillscope::kReplayPlay);
        ctx->Yield(2);
        IM_CHECK(contains(vars.text, "after update 0 of 13, playing at 10 updates a second"));
        vars.now = at(500);
        ctx->Yield(2);
        IM_CHECK(contains(vars.text, "after update 5 of 13, playing"));
        click(fillscope::kReplayPause);
        vars.now = at(900);
        ctx->Yield(2);
        IM_CHECK(contains(vars.text, "after update 5 of 13\n"));  // paused: no further, and not playing
        // Played on from 5; a step of the user's own pauses it.
        click(fillscope::kReplayPlay);
        vars.now = at(1200);
        ctx->Yield(2);
        IM_CHECK(contains(vars.text, "after update 8 of 13, playing"));
        click(fillscope::kReplayForward);
        vars.now = at(3000);
        ctx->Yield(2);
        IM_CHECK(contains(vars.text, "after update 9 of 13\n"));
        // A new rate goes on from where it is: 10 a second from 9 to 10, then 100 a second from there.
        click(fillscope::kReplayPlay);
        vars.now = at(3100);
        ctx->Yield(2);
        IM_CHECK(contains(vars.text, "after update 10 of 13, playing at 10 updates a second"));
        click("###replay_rate100");
        vars.now = at(3120);
        ctx->Yield(2);
        IM_CHECK(contains(vars.text, "after update 12 of 13, playing at 100 updates a second"));
        // To the end, where it stops; play again starts from the start.
        vars.now = at(10000);
        ctx->Yield(2);
        IM_CHECK(contains(vars.text, "after update 13 of 13\n"));
        IM_CHECK(ctx->ItemExists(std::format("**/{}", fillscope::kReplayPlay).c_str()));
        click(fillscope::kReplayPlay);
        ctx->Yield(2);
        IM_CHECK(contains(vars.text, "after update 0 of 13, playing"));
        click(fillscope::kReplayPause);

        // The jumps, from the start: each trade in turn, and nothing past the last.
        const auto jumps_to = [&](const char* item, int update) {
            click(item);
            ctx->Yield(2);
            IM_CHECK(contains(vars.text, std::format("after update {} of 13", update)));
        };
        vars.app->replay_to(0);
        jumps_to(fillscope::kReplayNextTrade, 4);
        jumps_to(fillscope::kReplayNextTrade, 7);
        jumps_to(fillscope::kReplayNextTrade, 9);
        jumps_to(fillscope::kReplayNextTrade, 9);  // none after
        jumps_to(fillscope::kReplayPrevTrade, 7);
        vars.app->replay_to(0);
        jumps_to(fillscope::kReplayNextSlicer, 4);
        jumps_to(fillscope::kReplayNextSlicer, 7);
        jumps_to(fillscope::kReplayNextSlicer, 7);  // 9 isn't the slicer's
        jumps_to(fillscope::kReplayPrevSlicer, 4);
        // The jumps follow the ticker shown: ticker 2's one trade, the slicer's.
        click("2##ticker");
        vars.app->replay_to(0);
        jumps_to(fillscope::kReplayNextSlicer, 12);

        // What happened: the request behind the update on screen, its updates, the one on screen
        // marked, and its sender's next request; and the jumps by request, past one that made no
        // update (5, the slicer's cancel of a piece already filled).
        vars.app->replay_to(5);
        ctx->Yield(2);
        IM_CHECK(contains(vars.text, "What happened, from the session's records"));
        IM_CHECK(contains(vars.text, "Request 4: the slicer (client 6) sent a buy of 10 at 105 on ticker 1, which made 2 updates:"));
        IM_CHECK(contains(vars.text, "  4: trade: 10 traded at 105, the slicer (client 6) buying from the market maker (client 1)'s order 1"));
        IM_CHECK(contains(vars.text, "> 5: order 1 removed: the trade filled it"));
        IM_CHECK(contains(vars.text, "Then, request 5: the slicer (client 6) tried to cancel an order on ticker 1, rejected: it was "
                                     "already gone."));
        jumps_to(fillscope::kReplayPrevRequest, 4);
        jumps_to(fillscope::kReplayPrevRequest, 3);
        jumps_to(fillscope::kReplayNextRequest, 4);
        jumps_to(fillscope::kReplayNextRequest, 6);
        vars.app->replay_to(0);
        ctx->Yield(2);
        IM_CHECK(contains(vars.text, "What happened: before the first request."));
    });

    // A replay without a truth draws its trades, none coloured, and says why.
    replay_test(engine, "a_replay_without_a_truth_says_it_has_none", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        vars.app->open_replay("runs/plain.dat", {.recording = "requests.rec", .updates = fillscope::test::session_updates()});
        vars.app->show_ticker(1);
        vars.app->replay_to(13);
        ctx->SetRef(fillscope::kViewerWindow);
        ctx->Yield(2);
        IM_CHECK(contains(vars.text, "No truth for this replay, so no colours"));
        IM_CHECK(!contains(vars.text, "Orange"));
        IM_CHECK(!contains(vars.text, "(slicer)"));
        IM_CHECK_EQ(std::string{ctx->ItemInfo("**/##trade4").DebugLabel}, std::string{"4: 10 at 105, none"});
        IM_CHECK_EQ(std::string{ctx->ItemInfo("**/##trade7").DebugLabel}, std::string{"7: 5 at 105, none"});
        // The trades' jumps, but not the slicer's, nor the requests': there is no truth to say which
        // are its, nor steps to say who sent what.
        IM_CHECK(ctx->ItemExists(std::format("**/{}", fillscope::kReplayNextTrade).c_str()));
        IM_CHECK(!ctx->ItemExists(std::format("**/{}", fillscope::kReplayNextSlicer).c_str()));
        IM_CHECK(!contains(vars.text, "What happened"));
    });

    // A long replay, going anywhere in it in any order, shows the books as if played from update 1:
    // a jump back is rebuilt from the nearest checkpoint, a step forward applied. Its update 3,500
    // can't be applied, so past it the books stay after 3,499, stuck, as a replay played through does.
    replay_test(engine, "a_long_replay_goes_anywhere_as_if_played_from_the_start", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        constexpr std::uint64_t kUpdates = 5'000;
        constexpr std::uint64_t kBad = 3'500;
        fillscope::test::Exchange exchange{7};
        fillscope::MarketDataFile replay{.recording = "runs/long.rec", .updates = {}};
        for (std::uint64_t seq = 1; seq <= kUpdates; ++seq) {
            replay.updates.push_back(seq == kBad ? MarketUpdate{.seq = seq, .type = UpdateType::cancel, .order_id = 999'999,
                                                                .ticker = 0, .side = Side::buy, .price = 1}
                                                 : exchange.next());
            replay.updates.back().seq = seq;
        }
        // Where to go, in this order: back, forward a step, across checkpoints both ways, past the
        // update that can't be applied, and back. Each compared with the books played from update 1.
        const std::vector<std::uint64_t> visits{4'000, 2'500, 2'501, 2'520, 100, 1'024, 1'023, 1'025, 3'000, 2'048,
                                                3'499, kBad, 4'999, kUpdates, 3'498, 1, 0, 3'072, 600, 2'047};
        std::vector<std::uint64_t> in_order = visits;
        std::ranges::sort(in_order);
        std::map<std::uint64_t, fillscope::BookBuilder> played;  // the books after each visited update, from update 1
        fillscope::BookBuilder books;
        std::uint64_t next = 1;
        for (const std::uint64_t seq : in_order) {
            for (; next <= seq; ++next) {
                if (books.status() != fillscope::BookBuilder::Status::recovering) {
                    books.on_incremental(replay.updates[next - 1]);
                }
            }
            played.insert_or_assign(seq, books);
        }
        vars.app->open_replay("runs/long.dat", std::move(replay));
        for (const std::uint64_t seq : visits) {
            vars.app->replay_to(seq);
            const fillscope::ViewerState& state = vars.app->state();
            const fillscope::BookBuilder& shown = state.books;
            const fillscope::BookBuilder& expected = played.at(seq);
            IM_CHECK_EQ(shown.status(), expected.status());
            IM_CHECK_EQ(shown.last_seq(), expected.last_seq());
            IM_CHECK_EQ(shown.updates_received(), expected.updates_received());
            IM_CHECK(shown.recovering_because() == expected.recovering_because());
            for (std::uint32_t ticker = 0; ticker < fillscope::BookBuilder::kTickers; ++ticker) {
                IM_CHECK(shown.book(ticker).orders() == expected.book(ticker).orders());
                IM_CHECK(shown.last_trade(ticker) == expected.last_trade(ticker));
                for (const fillscope::Order& order : expected.book(ticker).orders()) {
                    IM_CHECK(std::ranges::equal(fillscope::shown_history(state, ticker, order.order_id),
                                                expected.history(ticker, order.order_id)));
                }
            }
        }
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
    // No arguments: the live part. --replays: the other. A name: any test of that name, in either.
    const bool replays = argc > 1 && std::string_view{argv[1]} == "--replays";
    const char* filter = argc > (replays ? 2 : 1) ? argv[replays ? 2 : 1] : "all";
    g_part = replays ? Part::replays : std::string_view{filter} == "all" ? Part::live : Part::both;

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
