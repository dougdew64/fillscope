// The demos. The first: lesson 6 of the course, "How a market maker provides liquidity", played in
// the viewer as a person would play it: chosen from the drop-down in the answer pane, then stepped through
// with > to its end. At every frame it checks what is on screen against the lesson's answer, read
// on its own (load_answer, every claim checked again): where the pane says the user is, the step's
// question or its explanation, every claim marked true, and the book, one row per order resting
// after the step, rebuilt here from the market data rather than taken from the viewer.
//
// The lesson is played first, through the real answer tool and exchange_replay, into a scratch
// directory, so the demo doesn't depend on the course having been built.
//
// The second, --live: the viewer on the real exchange, as the VS Code compound "viewer + exchange +
// client" runs them, for 30 seconds from just before the first orders arrive. The demo starts the
// exchange (on ports of its own) and the viewer, then the client, told to warm up for a second, so
// trading begins about two seconds in. Then, at set times, it shows another ticker's book, points
// at an order and stops pointing, and does both again. At each it checks the viewer's books
// against its own, built from the same market data joined on its own: equal, as of the viewer's
// last update, and every order shown with its row; and where the viewer says the user points.
//
// The third, --time-shift: the same viewer on the real exchange, time-shifted (feed/time_shift.h).
// It pauses, and the books stand still while the updates keep arriving and are kept; points at the
// order that arrived last of those drawn, in the still book; steps back, an update at a time, until
// that order hadn't arrived (the bar: never seen), and forward one to see it arrive; plays on from
// there, behind live; and returns to live. Checked as the live demo is, at the update shown.
//
//   viewer_demo FILLSCOPE_ANSWER COURSE_DIR [--watch | --record FILE.mp4]
//   viewer_demo --live EXCHANGE_MAIN TRADING_MAIN [--watch | --record FILE.mp4]
//   viewer_demo --time-shift EXCHANGE_MAIN TRADING_MAIN [--watch | --record FILE.mp4]
//
// In the tests, headless (the Test Engine's null backend) and at full speed. With --watch, in a
// window, at a pace a person can follow: the mouse moves to each control before it clicks, and each
// frame stays on screen for a few seconds. With --record, the same, and recorded as a video: the
// Test Engine reads each frame back from the window and pipes it to ffmpeg, so the video holds the
// window alone, its drawn mouse included. The run is the same every way, checks included (in the
// tests, the live demo waits for the orders it needs rather than for the set times). Exits 0 only
// if the demo ran, every check passed, and any recording was written.

#include "answer/course.h"
#include "feed/book.h"
#include "feed/book_builder.h"
#include "feed/feed_socket.h"
#include "feed/kept_file.h"
#include "support/test_support.h"
#include "viewer/answer_state.h"
#include "viewer/viewer_app.h"
#include "viewer/viewer_view.h"
#include "viewer/window_size.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_te_context.h>
#include <imgui_te_engine.h>
#include <imgui_app.h>
#include <nlohmann/json.hpp>

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr std::string_view kLessonScenario = "06-market-maker.json";
// Recorded for sharing (LinkedIn's usual 16:9 landscape), the window is smaller than the viewer's,
// so its text is larger in the video, and the pace quicker: a viewer in a hurry pauses the video to
// read a frame in full.
constexpr fillscope::WindowSize kRecordingSize{.width = 1280, .height = 720};

std::filesystem::path scratch_root() {
    return std::filesystem::temp_directory_path() / std::format("fillscope-viewer-demo-{}", ::getpid());
}

// What the demo plays, set up before the Test Engine starts.
struct Demo {
    fillscope::Course course;
    std::filesystem::path built;           // where the lesson's answer was written
    std::size_t lesson = 0;                // its place in the course, from 1
    std::optional<fillscope::LoadedAnswer> expected;  // the lesson's answer, read on its own
    std::optional<std::filesystem::path> record;      // the video to write, if one is wanted
    float seconds_per_frame = 0.0f;                   // how long each frame stays on screen
    std::size_t frame = 0;                            // the frame shown while pointing
};
Demo g_demo;

// The live demo's programs and market data, set up before the Test Engine starts.
struct Live {
    std::string exchange_main;
    std::string trading_main;
    std::optional<fillscope::test::Program> exchange;
    std::optional<fillscope::test::Program> client;
    std::optional<fillscope::FeedSocket> incremental;  // the viewer's two streams
    std::optional<fillscope::FeedSocket> snapshot;
    std::optional<fillscope::FeedSocket> own;           // the demo's own, for its checks
    std::vector<fillscope::MarketUpdate> updates;       // every update the demo's own received
    std::optional<std::string> own_error;               // its stream's last error, if it had one
    bool held = false;  // the viewer takes in nothing meanwhile: what is checked or clicked stays put
    std::chrono::steady_clock::time_point start;        // when the 30 seconds began
};
bool g_live_mode = false;        // --live or --time-shift: on the real exchange
bool g_time_shift_mode = false;  // --time-shift
Live g_live;
// Ports of the demo's own (test_port), so nothing else on the machine using the exchange's usual
// ones (a lockstep session in VS Code, another run of the tests) reaches it.
const std::uint16_t kOrderPort = fillscope::test::test_port(0);
const fillscope::Stream kIncremental{fillscope::kExchangeIncremental.group, fillscope::test::test_port(1)};
const fillscope::Stream kSnapshot{fillscope::kExchangeSnapshot.group, fillscope::test::test_port(2)};
constexpr auto kLiveLength = std::chrono::seconds{30};
constexpr auto kTimeShiftLength = std::chrono::seconds{32};

std::vector<std::string> with_endpoints(std::vector<std::string> args) {
    for (std::string arg : {std::string{"--order-port"}, std::to_string(kOrderPort), std::string{"--incremental"},
                            std::format("{}:{}", kIncremental.group, kIncremental.port), std::string{"--snapshot"},
                            std::format("{}:{}", kSnapshot.group, kSnapshot.port)}) {
        args.push_back(std::move(arg));
    }
    return args;
}

struct DemoVars {
    std::unique_ptr<fillscope::ViewerApp> app;
    std::string text;  // the text the viewer rendered last frame
};

// The viewer as `viewer --answer FILE --course DIR` starts it: the user's questions' file (empty
// here), and the course beside it, nothing chosen. Live, as plain `viewer` starts it.
void set_up(ImGuiTestContext*, DemoVars& vars) {
    vars.app = std::make_unique<fillscope::ViewerApp>(scratch_root() / "context.json");
    if (!g_live_mode) {
        vars.app->watch_answer(scratch_root() / ".fillscope" / "answer.json");
        vars.app->open_course(g_demo.course, g_demo.built);
    } else {
        // Live, as the viewer does: what is kept written to a file, so a moment has an address there.
        std::filesystem::create_directories(scratch_root() / "kept");
        if (auto writer = fillscope::KeptFileWriter::create(scratch_root() / "kept" / "viewer.kept")) {
            vars.app->keep_to(std::move(*writer));
        }
    }
}

// One frame of the viewer's loop, as in src/viewer/main.cpp, with the rendered text captured. Live,
// the demo's own stream is read too, after the viewer's.
void viewer_frame(ImGuiTestContext* ctx) {
    DemoVars& vars = ctx->GetVars<DemoVars>();
    const auto now = std::chrono::steady_clock::now();
    if (g_live_mode) {
        if (!g_live.held) {
            vars.app->receive(*g_live.incremental, *g_live.snapshot, now);
        }
        if (auto received = g_live.own->receive(g_live.updates, fillscope::ViewerApp::kMaxDatagramsPerPaint); received.error) {
            g_live.own_error = std::move(received.error);
        }
    }
    vars.app->poll_answer(now);
    ImGui::LogToBuffer();
    (void)vars.app->paint(now);
    vars.text = ImGui::GetCurrentContext()->LogBuffer.c_str();
    ImGui::LogFinish();
}

bool contains(const std::string& text, std::string_view part) { return text.find(part) != std::string::npos; }

// ffmpeg's arguments for an H.264 MP4, which players and sites accept widely, or nothing if this
// ffmpeg has no H.264 encoder. Left to choose, ffmpeg falls back to MPEG-4 Part 2 where libx264 is
// missing (Fedora's ffmpeg-free), which many players can't play. libx264 is preferred; libopenh264
// takes a bit rate rather than a quality. The frames come in as the Test Engine sends them, at most
// $FPS a second but fewer when drawing and reading back a frame takes longer (the debug build), so
// each is stamped with the time it arrives, and the video made $FPS a second from them, a frame
// repeated where none came: it plays in the time the demo took.
std::optional<std::string> h264_arguments(const std::string& ffmpeg) {
    const std::string encoders = fillscope::test::run_command(fillscope::test::shell_quote(ffmpeg) + " -hide_banner -encoders").output;
    const std::string input =
        "-hide_banner -loglevel error -use_wallclock_as_timestamps 1 -f rawvideo -pix_fmt rgba -s $WIDTHx$HEIGHT -i - -threads 0 -y ";
    const std::string output = " -fps_mode cfr -r $FPS -pix_fmt yuv420p -movflags +faststart $OUTPUT";
    if (contains(encoders, " libx264 ")) {
        return input + "-c:v libx264 -preset medium -crf 20" + output;
    }
    if (contains(encoders, " libopenh264 ")) {
        return input + "-c:v libopenh264 -b:v 4M" + output;
    }
    return std::nullopt;
}

// Moves the mouse off the controls, so no tooltip covers the frame, but near the next one it clicks,
// so each move is short: just right of the answer's controls, or, before they are drawn, just
// below the drop-down. (The window fills the screen: there is no space outside it.)
void mouse_aside(ImGuiTestContext* ctx) {
    const std::string end = std::format("**/{}", fillscope::kAnswerEnd);
    if (ctx->ItemExists(end.c_str())) {
        const ImRect controls = ctx->ItemInfo(end.c_str()).RectFull;
        ctx->MouseMoveToPos(ImVec2{controls.Max.x + controls.GetWidth() * 3.0f, controls.GetCenter().y});
    } else {
        const ImRect choice = ctx->ItemInfo(std::format("**/{}", fillscope::kLessonChoice).c_str()).RectFull;
        ctx->MouseMoveToPos(ImVec2{choice.GetCenter().x, choice.Max.y + choice.GetHeight() * 3.0f});
    }
}

// The lesson's book after `frame`, rebuilt from its market data: updates 1 to the frame's
// books_after, those of the lesson's ticker.
fillscope::Book book_after(const fillscope::LoadedAnswer& answer, const fillscope::AnswerFrame& frame) {
    fillscope::Book book;
    for (std::uint64_t seq = 1; seq <= frame.books_after; ++seq) {
        const fillscope::MarketUpdate& update = answer.market_data.updates[seq - 1];
        if (update.ticker == answer.scenario.ticker) {
            (void)book.apply(update);
        }
    }
    return book;
}

// Checks what the viewer shows at `index` against the lesson's answer.
void check_frame(ImGuiTestContext* ctx, std::size_t index) {
    DemoVars& vars = ctx->GetVars<DemoVars>();
    const fillscope::LoadedAnswer& answer = *g_demo.expected;
    const fillscope::AnswerFrame& frame = answer.frames[index];
    const std::size_t steps = answer.scenario.steps.size();
    if (frame.step == 0) {
        IM_CHECK(contains(vars.text, std::format("introduction; {} steps", steps)));
        IM_CHECK(contains(vars.text, answer.scenario.intro));
        IM_CHECK(contains(vars.text, "Press > to begin."));
    } else {
        const fillscope::ScenarioStep& step = answer.scenario.steps[frame.step - 1];
        IM_CHECK(contains(vars.text, std::format("step {} of {}", frame.step, steps)));
        if (frame.asking) {
            IM_CHECK(contains(vars.text, step.ask));
            IM_CHECK(contains(vars.text, "Press > to see what happens."));
        } else {
            IM_CHECK(contains(vars.text, step.say));
            for (const fillscope::ClaimResult& claim : answer.checked[frame.step - 1].claims) {
                IM_CHECK(claim.holds);  // true by this program's own check, not only the tool's
                IM_CHECK(contains(vars.text, claim.says));
            }
            IM_CHECK(!contains(vars.text, "FALSE"));
        }
    }
    // The book: one row per resting order, and no other order in the viewer's books.
    const fillscope::Book expected_book = book_after(answer, frame);
    for (const fillscope::Order& order : expected_book.orders()) {
        IM_CHECK(ctx->ItemExists(std::format("**/{}", fillscope::order_label(order).c_str()).c_str()));
    }
    IM_CHECK(vars.app->state().books.book(answer.scenario.ticker).orders() == expected_book.orders());
}

// After a step that traded, the resting order the step changed last, if one rests: what the step's
// explanation is about, for the demo to point at. Nothing for a step that asks first, before it
// is played, or for one that made no trade.
std::optional<fillscope::Order> order_to_point_at(std::size_t index) {
    const fillscope::LoadedAnswer& answer = *g_demo.expected;
    const fillscope::AnswerFrame& frame = answer.frames[index];
    if (frame.step == 0 || frame.asking) {
        return std::nullopt;
    }
    const fillscope::CheckedStep& checked = answer.checked[frame.step - 1];
    const fillscope::Book book = book_after(answer, frame);
    bool traded = false;
    std::optional<fillscope::Order> changed;
    for (std::uint64_t seq = checked.first_update; seq < checked.first_update + checked.update_count; ++seq) {
        const fillscope::MarketUpdate& update = answer.market_data.updates[seq - 1];
        if (update.type == fillscope::UpdateType::trade) {
            traded = true;
        } else if (const fillscope::Order* order = book.find(update.order_id)) {
            changed = *order;
        }
    }
    return traded ? changed : std::nullopt;
}

// Points at `order`'s row, as a person would to show what the explanation is about, and checks
// the context bar then says so; after a pause, stops pointing, with the bar's x.
void point_at(ImGuiTestContext* ctx, const fillscope::Order& order) {
    DemoVars& vars = ctx->GetVars<DemoVars>();
    // Clicked near its left end, on its side: the row spans the table, and its middle can leave the
    // mouse over a column's heading, which lights up.
    const ImRect row = ctx->ItemInfo(std::format("**/{}", fillscope::order_label(order).c_str()).c_str()).RectFull;
    ctx->MouseMoveToPos(ImVec2{row.Min.x + row.GetHeight(), row.GetCenter().y});
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield(2);
    // Then just below the book's last row, where no tooltip covers the rows; the row stays marked,
    // and the bar says what is pointed at. (The bar has a line more now, so the rows have moved.)
    float below = 0.0f;
    for (const fillscope::Order& resting : book_after(*g_demo.expected, g_demo.expected->frames[g_demo.frame]).orders()) {
        below = std::max(below, ctx->ItemInfo(std::format("**/{}", fillscope::order_label(resting).c_str()).c_str()).RectFull.Max.y);
    }
    ctx->MouseMoveToPos(ImVec2{row.Min.x + row.GetHeight(), below + row.GetHeight()});
    IM_CHECK(contains(vars.text, std::format("Pointing at order {} of ticker {}", order.order_id, g_demo.expected->scenario.ticker)));
    ctx->Sleep(g_demo.seconds_per_frame);
    ctx->ItemClick(std::format("**/{}", fillscope::kClearPointButton).c_str());
    ctx->Yield(2);
    IM_CHECK(!contains(vars.text, "Pointing at"));
}

// Scrolls `pane` as a person would with a mouse's wheel, over the pane, a notch at a time: down
// (`notch` -1) to its end, or up (+1) to its top.
void wheel(ImGuiTestContext* ctx, ImGuiWindow* pane, float notch) {
    ctx->MouseMoveToPos(pane->Rect().GetCenter());
    const auto done = [&] { return notch < 0.0f ? pane->Scroll.y >= pane->ScrollMax.y : pane->Scroll.y <= 0.0f; };
    for (int notches = 0; !done() && notches < 50; ++notches) {
        ctx->MouseWheelY(notch);
        ctx->Yield(2);
        ctx->Sleep(0.25f);
    }
    IM_CHECK(done());
}

void play(ImGuiTestContext* ctx) {
    DemoVars& vars = ctx->GetVars<DemoVars>();
    IM_CHECK(contains(vars.text, "Choose a lesson"));  // the rest may be cut to fit
    ctx->Sleep(g_demo.seconds_per_frame / 2);

    const fillscope::Lesson& lesson = g_demo.course.lessons[g_demo.lesson - 1];
    ctx->ItemClick(std::format("**/{}", fillscope::kLessonChoice).c_str());
    ctx->Sleep(g_demo.seconds_per_frame * 1.5f);  // the list open, long enough to read a few lessons
    ctx->ItemClick(std::format("//$FOCUSED/{}. {}##choice{}", g_demo.lesson, lesson.title, g_demo.lesson).c_str());
    mouse_aside(ctx);
    ctx->Yield(2);

    // The answer pane: a child window, whose name Dear ImGui mangles, so it is found by its path.
    ImGuiWindow* pane = ctx->WindowInfo(std::format("//{}/{}", fillscope::kViewerWindow, fillscope::kAnswerPane).c_str()).Window;
    IM_CHECK(pane != nullptr);
    const std::size_t frames = g_demo.expected->frames.size();
    for (std::size_t index = 0; index < frames; ++index) {
        check_frame(ctx, index);
        if (ctx->IsError()) {
            return;  // the first failure says what's wrong; the frames after it would only repeat it
        }
        ctx->Sleep(g_demo.seconds_per_frame);  // skipped at full speed
        // A frame longer than the pane: scrolled to its end with the mouse's wheel, so a viewer sees
        // there is more, and shown three quarters as long again.
        if (pane->ScrollMax.y > 0.0f) {
            wheel(ctx, pane, -1.0f);
            ctx->Sleep(g_demo.seconds_per_frame * 0.75f);
        }
        if (const auto order = order_to_point_at(index)) {
            g_demo.frame = index;
            point_at(ctx, *order);
            if (ctx->IsError()) {
                return;
            }
        }
        if (index + 1 < frames) {
            // Back to the top with the wheel, where > is: left to itself, the Test Engine would
            // bring > into view by scrolling the pane with no mouse in sight.
            if (pane->Scroll.y > 0.0f) {
                wheel(ctx, pane, 1.0f);
            }
            ctx->ItemClick(std::format("**/{}", fillscope::kAnswerForward).c_str());
            mouse_aside(ctx);
            ctx->Yield(2);
        }
    }
}

// The demo, recorded from its first frame to its last when a video is wanted.
void play_the_lesson(ImGuiTestContext* ctx) {
    ctx->SetRef(fillscope::kViewerWindow);
    ctx->Yield(2);
    mouse_aside(ctx);  // before recording starts, so the video doesn't open on the mouse moving
    if (g_demo.record) {
        ImGuiCaptureArgs* args = ctx->CaptureArgs;
        ImStrncpy(args->InOutputFile, g_demo.record->c_str(), sizeof(args->InOutputFile));
        args->InPadding = 0.0f;
        // The viewer's window, which fills the screen; and every other window kept, since otherwise
        // the capture hides them, the drop-down's list among them. Its size as it is: the capture
        // otherwise trims it to a multiple of its own choosing.
        IM_CHECK(ctx->CaptureAddWindow(std::format("//{}", fillscope::kViewerWindow).c_str()));
        args->InFlags |= ImGuiCaptureFlags_IncludeOtherWindows;
        args->InSizeAlign = 1;
        IM_CHECK(ctx->CaptureBeginVideo());
    }
    play(ctx);
    if (g_demo.record) {
        IM_CHECK(ctx->CaptureEndVideo());
    }
}

// A book's orders as the viewer lists them: asks above bids, each side's best price nearest the
// middle, and within a price in the order they will fill.
std::vector<fillscope::Order> rows_of(const fillscope::Book& book) {
    std::vector<fillscope::Order> rows;
    const std::vector<fillscope::Level> asks = book.levels(fillscope::Side::sell);
    for (auto level = asks.rbegin(); level != asks.rend(); ++level) {
        rows.insert(rows.end(), level->orders.begin(), level->orders.end());
    }
    for (const fillscope::Level& level : book.levels(fillscope::Side::buy)) {
        rows.insert(rows.end(), level.orders.begin(), level.orders.end());
    }
    return rows;
}

// Holds the viewer's market data while it lives, so the books stay as they are across the frames a
// check or a click takes (the mouse's way to a row takes a fraction of a second); what arrives
// meanwhile waits in the sockets, as it does when a paint has taken its most, and is taken in after.
struct Hold {
    explicit Hold(ImGuiTestContext* ctx) {
        g_live.held = true;
        ctx->Yield(2);  // a frame drawn from the books as held
    }
    ~Hold() { g_live.held = false; }
    Hold(const Hold&) = delete;
    Hold& operator=(const Hold&) = delete;
};

// The book's rows the viewer drew in its last frame, as its rendered text has them, one line each
// ("| buy | [ 182 ] | 85 | #1 | [ 25 ] | 85 |"): each row's side and order id, top to bottom. Read
// from one frame's text, where asking the Test Engine about each row would take a frame a row.
struct DrawnRow {
    std::string side;
    std::uint64_t order_id = 0;
    friend bool operator==(const DrawnRow&, const DrawnRow&) = default;
};

std::vector<DrawnRow> drawn_rows(const std::string& text) {
    std::vector<DrawnRow> rows;
    const std::size_t header = text.find("| side | price |");
    if (header == std::string::npos) {
        return rows;
    }
    std::size_t line = text.find('\n', header);
    while (line != std::string::npos && text.compare(line + 1, 2, "| ") == 0) {
        const std::size_t end = text.find('\n', line + 1);
        std::vector<std::string> cells;
        std::string cell;
        for (const char c : text.substr(line + 1, end == std::string::npos ? std::string::npos : end - line - 1)) {
            if (c == '|') {
                cells.push_back(cell);
                cell.clear();
            } else if (c != ' ' && c != '[' && c != ']') {
                cell += c;
            }
        }
        // "", side, price, level qty, queue, order, qty
        if (cells.size() >= 6) {
            rows.push_back({cells[1], std::stoull(cells[5])});
        }
        line = end;
    }
    return rows;
}

std::vector<DrawnRow> as_rows(const std::vector<fillscope::Order>& orders) {
    std::vector<DrawnRow> rows;
    for (const fillscope::Order& order : orders) {
        rows.push_back({order.side == fillscope::Side::buy ? "buy" : "sell", order.order_id});
    }
    return rows;
}

// Live: the viewer's books against the demo's own, built from the updates its own stream received,
// 1 to the viewer's last: equal, and no update missed or recovered from. The shown book's rows are
// its orders, from the first, as many as the table has room for: drawn rows the book's first, in
// its order, none skipped.
void check_live(ImGuiTestContext* ctx) {
    DemoVars& vars = ctx->GetVars<DemoVars>();
    const Hold hold{ctx};
    const fillscope::BookBuilder& books = vars.app->state().books;
    // The demo's own stream may be a paint behind the viewer's.
    for (int paints = 0; g_live.updates.size() < books.last_seq() && paints < 100; ++paints) {
        ctx->Yield();
    }
    IM_CHECK_RETV(g_live.updates.size() >= books.last_seq(), void());
    IM_CHECK(books.status() == fillscope::BookBuilder::Status::live);
    IM_CHECK(books.recoveries().empty());
    IM_CHECK(!vars.app->state().feed_error);
    IM_CHECK(!g_live.own_error);
    fillscope::BookBuilder own;
    for (std::uint64_t seq = 1; seq <= books.last_seq(); ++seq) {
        own.on_incremental(g_live.updates[seq - 1]);
    }
    for (std::uint32_t ticker = 0; ticker < fillscope::BookBuilder::kTickers; ++ticker) {
        IM_CHECK(books.book(ticker).orders() == own.book(ticker).orders());
    }
    // And what the viewer keeps of it (feed/time_shift.h) gives the same books, as of the same update:
    // the newest, following live; time-shifted, one behind it.
    if (vars.app->state().time_shift) {
        IM_CHECK(vars.app->kept().newest() >= books.last_seq());
        IM_CHECK(vars.app->state().time_shift->shown == books.last_seq());
    } else {
        IM_CHECK(vars.app->kept().newest() == books.last_seq());
    }
    if (const auto moment = vars.app->kept().moment(books.last_seq())) {
        for (std::uint32_t ticker = 0; ticker < fillscope::BookBuilder::kTickers; ++ticker) {
            IM_CHECK(moment->books[ticker].orders() == own.book(ticker).orders());
        }
    } else {
        IM_ERRORF("the viewer kept no moment as of update %llu: %s", static_cast<unsigned long long>(books.last_seq()),
                  moment.error().c_str());
    }
    const std::vector<DrawnRow> rows = as_rows(rows_of(books.book(vars.app->state().shown_ticker)));
    const std::vector<DrawnRow> drawn = drawn_rows(vars.text);
    IM_CHECK(rows.empty() || !drawn.empty());
    IM_CHECK(drawn.size() <= rows.size() && std::equal(drawn.begin(), drawn.end(), rows.begin()));
}

// Live: waits until `seconds` into the 30 seconds, when a person would do the next thing, and in the
// tests, which don't wait for the times, until `ready` holds instead; either way, until it holds,
// for at most 30 seconds more, with the exchange and the client still running.
template <typename Ready>
void wait_live(ImGuiTestContext* ctx, float seconds, Ready ready) {
    const bool paced = ctx->EngineIO->ConfigRunSpeed != ImGuiTestRunSpeed_Fast;
    const auto at = g_live.start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<float>(seconds));
    const auto deadline = std::max(at, std::chrono::steady_clock::now()) + std::chrono::seconds{30};
    while ((paced && std::chrono::steady_clock::now() < at) || !ready()) {
        IM_CHECK_RETV(std::chrono::steady_clock::now() < deadline, void());
        IM_CHECK_RETV(g_live.exchange->running() && g_live.client->running(), void());
        ctx->Yield();
    }
}

// Moves the mouse aside, just right of the ticker buttons: off the books, so no tooltip covers them
// and no row lights up, however the book grows.
void mouse_aside_live(ImGuiTestContext* ctx) {
    const ImRect last = ctx->ItemInfo(std::format("**/{}", fillscope::ticker_label(fillscope::BookBuilder::kTickers - 1).c_str()).c_str()).RectFull;
    ctx->MouseMoveToPos(ImVec2{last.Max.x + last.GetWidth() * 3.0f, last.GetCenter().y});
}

// Live: the address the context gives what is pointed at (the kept file, and the update the books
// shown are as of) rebuilds, from the file, the books the demo built from its own stream as of that
// update: what a question asked later would be about is exactly what was on the screen.
void check_kept_address() {
    std::ifstream in{scratch_root() / "context.json"};
    const nlohmann::json context = nlohmann::json::parse(in, nullptr, false);
    IM_CHECK_RETV(!context.is_discarded() && context["point"]["keptAddress"].is_object(), void());
    const std::string file = context["point"]["keptAddress"]["file"].get<std::string>();
    const auto seq = context["point"]["keptAddress"]["seq"].get<std::uint64_t>();
    const auto kept = fillscope::read_kept_file(file);
    IM_CHECK_RETV(kept.has_value(), void());
    const auto moment = kept->moment(seq);
    IM_CHECK_RETV(moment.has_value(), void());
    IM_CHECK_RETV(g_live.updates.size() >= seq, void());
    fillscope::BookBuilder own;
    for (std::uint64_t next = 1; next <= seq; ++next) {
        own.on_incremental(g_live.updates[next - 1]);
    }
    for (std::uint32_t ticker = 0; ticker < fillscope::BookBuilder::kTickers; ++ticker) {
        IM_CHECK(moment->books[ticker].orders() == own.book(ticker).orders());
    }
}

// Shows `ticker`'s book with its button, once it has at least `orders` resting.
void show_ticker(ImGuiTestContext* ctx, float seconds, std::uint32_t ticker, std::size_t orders) {
    DemoVars& vars = ctx->GetVars<DemoVars>();
    wait_live(ctx, seconds, [&] { return vars.app->state().books.book(ticker).order_count() >= orders; });
    ctx->ItemClick(std::format("**/{}", fillscope::ticker_label(ticker).c_str()).c_str());
    mouse_aside_live(ctx);
    ctx->Yield(2);
    IM_CHECK(vars.app->state().shown_ticker == ticker);
    check_live(ctx);
}

// The order of the row `fraction` of the way down the shown book's rows drawn (0: the top row):
// away from the middle, where the orders that trade are, so the one pointed at is likelier to stay.
std::optional<fillscope::Order> drawn_order(ImGuiTestContext* ctx, float fraction) {
    DemoVars& vars = ctx->GetVars<DemoVars>();
    const std::vector<fillscope::Order> rows = rows_of(vars.app->state().books.book(vars.app->state().shown_ticker));
    const std::size_t drawn = std::min(drawn_rows(vars.text).size(), rows.size());
    if (drawn == 0) {
        return std::nullopt;
    }
    return rows[std::min(drawn - 1, static_cast<std::size_t>(fraction * static_cast<float>(drawn)))];
}

// Points at the order `fraction` of the way down the rows drawn, clicked near its row's left end,
// and checks the bar says so; at `until`, stops pointing, with the bar's x. The order may be
// cancelled or filled meanwhile: the bar then says what became of it.
void point_live(ImGuiTestContext* ctx, float seconds, float fraction, float until) {
    DemoVars& vars = ctx->GetVars<DemoVars>();
    const std::uint32_t ticker = vars.app->state().shown_ticker;
    wait_live(ctx, seconds, [&] { return vars.app->state().books.book(ticker).order_count() > 0; });
    std::optional<Hold> hold{std::in_place, ctx};
    const auto chosen = drawn_order(ctx, fraction);
    IM_CHECK_RETV(chosen.has_value(), void());
    const fillscope::Order order = *chosen;
    const ImRect row = ctx->ItemInfo(std::format("**/{}", fillscope::order_label(order).c_str()).c_str()).RectFull;
    ctx->MouseMoveToPos(ImVec2{row.Min.x + row.GetHeight(), row.GetCenter().y});
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield(2);
    const fillscope::PointTarget expected{fillscope::OrderRef{.ticker = ticker, .order_id = order.order_id}};
    IM_CHECK(vars.app->selection().pointed() == expected);
    IM_CHECK(contains(vars.text, std::format("Pointing at order {} of ticker {}", order.order_id, ticker)));
    check_kept_address();
    hold.reset();
    mouse_aside_live(ctx);
    check_live(ctx);
    wait_live(ctx, until, [] { return true; });
    ctx->ItemClick(std::format("**/{}", fillscope::kClearPointButton).c_str());
    mouse_aside_live(ctx);
    ctx->Yield(2);
    IM_CHECK(!contains(vars.text, "Pointing at"));
}

// Live: the mouse aside and, when a video is wanted, its recording begun; then the client started,
// told to warm up for a second, so trading begins about two seconds in. The demo's time starts here.
void begin_live(ImGuiTestContext* ctx) {
    ctx->SetRef(fillscope::kViewerWindow);
    ctx->Yield(2);
    mouse_aside_live(ctx);
    if (g_demo.record) {
        ImGuiCaptureArgs* args = ctx->CaptureArgs;
        ImStrncpy(args->InOutputFile, g_demo.record->c_str(), sizeof(args->InOutputFile));
        args->InPadding = 0.0f;
        IM_CHECK(ctx->CaptureAddWindow(std::format("//{}", fillscope::kViewerWindow).c_str()));
        args->InFlags |= ImGuiCaptureFlags_IncludeOtherWindows;
        args->InSizeAlign = 1;
        IM_CHECK(ctx->CaptureBeginVideo());
    }
    g_live.start = std::chrono::steady_clock::now();
    g_live.client.emplace(with_endpoints({g_live.trading_main, "5", "RANDOM", "--warm-up-seconds", "1"}),
                          scratch_root());
    IM_CHECK(g_live.client->started());
}

// Live: at `length`, the last check, and the recording's end.
void end_live(ImGuiTestContext* ctx, std::chrono::seconds length) {
    wait_live(ctx, std::chrono::duration<float>(length).count(), [] { return true; });
    check_live(ctx);
    if (g_demo.record) {
        IM_CHECK(ctx->CaptureEndVideo());
    }
}

// The live demo, recorded from just before the client starts to 30 seconds later.
void play_live(ImGuiTestContext* ctx) {
    begin_live(ctx);
    DemoVars& vars = ctx->GetVars<DemoVars>();
    // Ticker 0's book fills; then another ticker's, an order pointed at in each.
    wait_live(ctx, 0.0f, [&] { return vars.app->state().books.updates_received() > 0; });
    show_ticker(ctx, 8.0f, 0, 1);  // already shown: a check, and the button lit
    show_ticker(ctx, 9.0f, 3, 3);
    point_live(ctx, 12.0f, 0.0f, 16.0f);
    show_ticker(ctx, 18.0f, 6, 3);
    point_live(ctx, 21.0f, 0.5f, 25.0f);
    show_ticker(ctx, 27.0f, 0, 1);
    end_live(ctx, kLiveLength);
}

// Clicks made at once, where the mouse already rests: the Test Engine's watchable speeds wait about
// each click as a person's hand would, which, a step at a time, made stepping back take seconds.
// The demo's own pace (linger) keeps each step on screen.
struct QuickClicks {
    explicit QuickClicks(ImGuiTestContext* ctx) : io{*ctx->EngineIO}, speed{io.ConfigRunSpeed} { io.ConfigRunSpeed = ImGuiTestRunSpeed_Fast; }
    ~QuickClicks() { io.ConfigRunSpeed = speed; }
    QuickClicks(const QuickClicks&) = delete;
    QuickClicks& operator=(const QuickClicks&) = delete;
    ImGuiTestEngineIO& io;
    ImGuiTestRunSpeed speed;
};

// Live, watched or recorded: frames for `seconds` of real time, so a step stays on screen; in the
// tests, nothing.
void linger(ImGuiTestContext* ctx, float seconds) {
    if (!g_demo.record && ctx->EngineIO->ConfigRunSpeed == ImGuiTestRunSpeed_Fast) {
        return;
    }
    const auto until = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<float>(seconds));
    while (std::chrono::steady_clock::now() < until) {
        ctx->Yield();
    }
}

// Clicks a time-shifting control, the mouse aside after, and lets the frame show it.
void click_shift(ImGuiTestContext* ctx, const char* control) {
    ctx->ItemClick(std::format("**/{}", control).c_str());
    mouse_aside_live(ctx);
    ctx->Yield(2);
}

// Of the shown book's rows in view, the order that arrived last (its add the latest update), and that
// update: the one fewest steps back from the update shown. Nothing if none in view was seen arriving.
// In view, not just drawn: the table lays out a row or two past its visible edge, which the rendered
// text includes, and a click there would land outside the window.
std::optional<std::pair<fillscope::Order, std::uint64_t>> latest_arrival(ImGuiTestContext* ctx) {
    DemoVars& vars = ctx->GetVars<DemoVars>();
    const fillscope::BookBuilder& books = vars.app->state().books;
    const std::uint32_t ticker = vars.app->state().shown_ticker;
    const std::vector<fillscope::Order> rows = rows_of(books.book(ticker));
    std::vector<std::pair<fillscope::Order, std::uint64_t>> arrivals;  // newest first
    for (std::size_t row = 0; row < std::min(drawn_rows(vars.text).size(), rows.size()); ++row) {
        const auto history = fillscope::shown_history(vars.app->state(), ticker, rows[row].order_id);
        if (!history.empty() && !history.front().from_snapshot) {
            arrivals.emplace_back(rows[row], history.front().seq);
        }
    }
    std::ranges::sort(arrivals, std::greater<>{}, [](const auto& arrival) { return arrival.second; });
    for (const auto& arrival : arrivals) {
        const ImGuiTestItemInfo info = ctx->ItemInfo(std::format("**/{}", fillscope::order_label(arrival.first).c_str()).c_str());
        if (info.Window != nullptr && info.Window->InnerClipRect.Contains(info.RectFull)) {
            return arrival;
        }
    }
    return std::nullopt;
}

// The time-shifting demo, recorded from just before the client starts to 32 seconds later.
void play_time_shift(ImGuiTestContext* ctx) {
    begin_live(ctx);
    DemoVars& vars = ctx->GetVars<DemoVars>();
    const auto& state = vars.app->state();
    wait_live(ctx, 0.0f, [&] { return state.books.updates_received() > 0; });
    show_ticker(ctx, 5.0f, 3, 6);  // a busy book
    // Paused: the books stand still while the updates keep arriving, and are kept. The mouse waits on
    // pause, and clicks just after an order arrives in a row in view, so that it arrived only a few
    // updates before the moment held: a few steps back show it arriving.
    wait_live(ctx, 7.0f, [] { return true; });
    ctx->MouseMove(std::format("**/{}", fillscope::kShiftPause).c_str());
    wait_live(ctx, 8.0f, [&] {
        const fillscope::MarketUpdate* newest = vars.app->kept().newest() ? vars.app->kept().update(*vars.app->kept().newest()) : nullptr;
        if (newest == nullptr || newest->type != fillscope::UpdateType::add || newest->ticker != state.shown_ticker) {
            return false;
        }
        // Its row in view, as latest_arrival will want it, from this frame's text alone (asking the Test
        // Engine about an item takes frames, during which more updates arrive): drawn, and not among
        // the last few rows drawn, which can be past the table's visible edge.
        const std::vector<DrawnRow> drawn = drawn_rows(vars.text);
        return std::ranges::any_of(drawn.begin(), drawn.size() > 4 ? drawn.end() - 4 : drawn.begin(),
                                   [&](const DrawnRow& row) { return row.order_id == newest->order_id; });
    });
    {
        const QuickClicks quick{ctx};  // the mouse waits on pause: the click comes at once
        ctx->MouseClick(ImGuiMouseButton_Left);
    }
    mouse_aside_live(ctx);
    ctx->Yield(2);
    IM_CHECK_RETV(state.time_shift.has_value(), void());
    check_live(ctx);
    // In the still book, the order that arrived last of those drawn, pointed at.
    wait_live(ctx, 10.0f, [&] { return state.time_shift->newest > state.time_shift->shown; });
    // Paused, the books shown don't move: no need to hold the market data for the click.
    const auto arrival = latest_arrival(ctx);
    IM_CHECK_RETV(arrival.has_value(), void());
    const ImRect row = ctx->ItemInfo(std::format("**/{}", fillscope::order_label(arrival->first).c_str()).c_str()).RectFull;
    ctx->MouseMoveToPos(ImVec2{row.Min.x + row.GetHeight(), row.GetCenter().y});
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield(2);
    const auto& [order, added] = *arrival;
    const std::string pointing = std::format("Pointing at order {} of ticker {}", order.order_id, state.shown_ticker);
    IM_CHECK(contains(vars.text, pointing));
    mouse_aside_live(ctx);
    check_kept_address();
    // Back, an update at a time, to the one before it arrived: the bar says it was never seen, and
    // its row is gone; then forward one, and it arrives.
    wait_live(ctx, 13.0f, [] { return true; });
    IM_CHECK_LE(state.time_shift->shown - added, 20u);  // a few steps, so the demo keeps its time
    ctx->MouseMove(std::format("**/{}", fillscope::kShiftBack).c_str());
    while (state.time_shift->shown >= added) {
        {
            const QuickClicks quick{ctx};  // the mouse rests on <: a click a step
            ctx->MouseClick(ImGuiMouseButton_Left);
        }
        ctx->Yield(2);
        linger(ctx, 0.2f);
    }
    mouse_aside_live(ctx);
    ctx->Yield(2);
    IM_CHECK_EQ(state.time_shift->shown, added - 1);
    IM_CHECK(!ctx->ItemExists(std::format("**/{}", fillscope::order_label(order).c_str()).c_str()));
    IM_CHECK(contains(vars.text, pointing + ": never seen"));
    check_live(ctx);
    wait_live(ctx, 16.0f, [] { return true; });
    click_shift(ctx, fillscope::kShiftForward);
    IM_CHECK_EQ(state.time_shift->shown, added);
    IM_CHECK(ctx->ItemExists(std::format("**/{}", fillscope::order_label(order).c_str()).c_str()));
    IM_CHECK(contains(vars.text, pointing + ": " + std::string{to_string(order.side)}));
    check_live(ctx);
    check_kept_address();
    // Played on from there, at the pace the updates arrived, behind live.
    wait_live(ctx, 19.0f, [] { return true; });
    click_shift(ctx, fillscope::kShiftPlay);
    IM_CHECK(state.time_shift->playing);
    wait_live(ctx, 25.0f, [&] { return state.time_shift->shown > added + 5; });
    IM_CHECK(state.time_shift->playing);
    IM_CHECK_GT(state.time_shift->newest, state.time_shift->shown);
    check_live(ctx);
    // And back to live.
    wait_live(ctx, 27.0f, [] { return true; });
    click_shift(ctx, fillscope::kShiftLive);
    IM_CHECK(!state.time_shift.has_value());
    check_live(ctx);
    end_live(ctx, kTimeShiftLength);
}

// Starts the exchange, once its port is open, and joins its streams for the viewer and for the demo.
bool prepare_live(const std::string& exchange_main, const std::string& trading_main) {
    g_live.exchange_main = std::filesystem::absolute(exchange_main);
    g_live.trading_main = std::filesystem::absolute(trading_main);
    std::filesystem::create_directories(scratch_root());
    if (fillscope::test::tcp_port_open(kOrderPort)) {
        std::fprintf(stderr, "viewer_demo: something already listens on the demo's order port, %u\n", kOrderPort);
        return false;
    }
    for (auto [stream, socket] : {std::pair{&kIncremental, &g_live.incremental}, std::pair{&kSnapshot, &g_live.snapshot},
                                  std::pair{&kIncremental, &g_live.own}}) {
        auto joined = fillscope::FeedSocket::join(*stream);
        if (!joined) {
            std::fprintf(stderr, "viewer_demo: can't join %s:%u: %s\n", stream->group.c_str(), stream->port, joined.error().message().c_str());
            return false;
        }
        socket->emplace(std::move(*joined));
    }
    g_live.exchange.emplace(with_endpoints({g_live.exchange_main}), scratch_root());
    // Its port opens in about a second; the limit allows for the sanitizer build and a loaded machine.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    while (!fillscope::test::tcp_port_open(kOrderPort)) {
        if (!g_live.exchange->running() || std::chrono::steady_clock::now() > deadline) {
            std::fprintf(stderr, "viewer_demo: the exchange didn't open port %u\n", kOrderPort);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    return true;
}

// Plays the lesson through the answer tool into the scratch directory, and reads it back.
bool prepare(const std::string& answer_tool, const std::filesystem::path& course_dir) {
    auto course = fillscope::read_course_file(course_dir / "course.json");
    if (!course) {
        std::fprintf(stderr, "viewer_demo: %s\n", course.error().c_str());
        return false;
    }
    g_demo.course = std::move(*course);
    g_demo.built = scratch_root() / "course";
    for (std::size_t i = 0; i < g_demo.course.lessons.size(); ++i) {
        if (g_demo.course.lessons[i].scenario == kLessonScenario) {
            g_demo.lesson = i + 1;
        }
    }
    if (g_demo.lesson == 0) {
        std::fprintf(stderr, "viewer_demo: the course has no lesson %s\n", std::string{kLessonScenario}.c_str());
        return false;
    }
    const std::filesystem::path answer = fillscope::lesson_answer(g_demo.built, g_demo.course.lessons[g_demo.lesson - 1]);
    const auto result = fillscope::test::run_command(
        std::format("{} {} --answer {}", fillscope::test::shell_quote(answer_tool),
                    fillscope::test::shell_quote((course_dir / kLessonScenario).string()),
                    fillscope::test::shell_quote(answer.string())));
    if (result.status != 0) {
        std::fprintf(stderr, "viewer_demo: the answer tool failed: %s\n", result.output.c_str());
        return false;
    }
    auto loaded = fillscope::load_answer(answer);
    if (!loaded) {
        std::fprintf(stderr, "viewer_demo: %s\n", loaded.error().c_str());
        return false;
    }
    g_demo.expected = std::move(*loaded);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    // --live or --time-shift first, then the two programs' paths, as the lesson's two are given.
    g_time_shift_mode = argc > 1 && std::string_view{argv[1]} == "--time-shift";
    g_live_mode = g_time_shift_mode || (argc > 1 && std::string_view{argv[1]} == "--live");
    if (g_live_mode) {
        --argc;
        ++argv;
    }
    const bool record = argc == 5 && std::string_view{argv[3]} == "--record";
    const bool watch = record || (argc == 4 && std::string_view{argv[3]} == "--watch");
    if (argc != 3 && !watch) {
        std::fprintf(stderr, "usage: viewer_demo FILLSCOPE_ANSWER COURSE_DIR [--watch | --record FILE.mp4]\n"
                             "       viewer_demo --live EXCHANGE_MAIN TRADING_MAIN [--watch | --record FILE.mp4]\n"
                             "       viewer_demo --time-shift EXCHANGE_MAIN TRADING_MAIN [--watch | --record FILE.mp4]\n");
        return 2;
    }
    std::string ffmpeg;
    std::optional<std::string> encoder;
    if (record) {
        g_demo.record = std::filesystem::absolute(argv[4]);
        const auto found = fillscope::test::run_command("command -v ffmpeg");
        ffmpeg = found.output.substr(0, found.output.find('\n'));
        if (found.status != 0 || ffmpeg.empty()) {
            std::fprintf(stderr, "viewer_demo: recording needs ffmpeg, which isn't on the PATH\n");
            return 1;
        }
        if (!(encoder = h264_arguments(ffmpeg))) {
            std::fprintf(stderr, "viewer_demo: recording needs an H.264 encoder (libx264 or libopenh264), which %s hasn't\n",
                         ffmpeg.c_str());
            return 1;
        }
        if (encoder->size() >= sizeof(ImGuiTestEngineIO::VideoCaptureEncoderParams)) {
            std::fprintf(stderr, "viewer_demo: ffmpeg's arguments are too long for the Test Engine\n");
            return 1;
        }
        if (g_demo.record->string().size() >= sizeof(ImGuiCaptureArgs::InOutputFile)) {
            std::fprintf(stderr, "viewer_demo: the video's path is too long\n");
            return 1;
        }
        std::filesystem::remove(*g_demo.record);
    }
    std::filesystem::remove_all(scratch_root());
    if (!(g_live_mode ? prepare_live(argv[1], argv[2]) : prepare(argv[1], argv[2]))) {
        return 1;
    }

    // Watched, a window on the screen at the viewer's own size; otherwise none, at the size the
    // viewer would have with no screen to fit.
    ImGuiApp* app = watch ? ImGuiApp_ImplGlfwGL3_Create() : ImGuiApp_ImplNull_Create();
    fillscope::WindowSize size = fillscope::window_size(0, 0);
    if (watch) {
        if (!glfwInit()) {
            std::fprintf(stderr, "viewer_demo: can't initialize GLFW (is there a display?)\n");
            return 1;
        }
        size = record ? kRecordingSize : fillscope::primary_window_size();
        // Recorded, a framebuffer of the window's own size, whatever the display's scale: the
        // capture reads the framebuffer in the window's coordinates, so on a display at scale 2 it
        // would otherwise record a quarter of the window, magnified. (On such a display the window
        // looks a little soft while it records; the video doesn't.)
        if (record) {
            glfwWindowHint(GLFW_SCALE_FRAMEBUFFER, GLFW_FALSE);
        }
        app->Vsync = true;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    fillscope::apply_viewer_look();
    const char* title = g_time_shift_mode ? "Fillscope demo: time-shifting"
                        : g_live_mode       ? "Fillscope demo: live trading"
                                            : "Fillscope demo: a market maker";
    if (!app->InitCreateWindow(app, title, ImVec2(static_cast<float>(size.width), static_cast<float>(size.height)))) {
        std::fprintf(stderr, "viewer_demo: can't create the window\n");
        return 1;
    }
    app->InitBackends(app);

    ImGuiTestEngine* engine = ImGuiTestEngine_CreateContext();
    ImGuiTestEngineIO& test_io = ImGuiTestEngine_GetIO(engine);
    // Watched: cinematic, the mouse slow enough to follow and a pause after each action, and each
    // frame shown four seconds. Recorded: normal speed, the mouse quick and no pauses, and two
    // seconds a frame. Sleep() waits in both; in the tests, Fast skips the waits and the moves.
    test_io.ConfigRunSpeed = record ? ImGuiTestRunSpeed_Normal : watch ? ImGuiTestRunSpeed_Cinematic : ImGuiTestRunSpeed_Fast;
    g_demo.seconds_per_frame = record ? 2.0f : 4.0f;
    test_io.ConfigNoThrottle = !watch;
    test_io.ConfigLogToTTY = true;
    test_io.ConfigVerboseLevel = ImGuiTestVerboseLevel_Info;
    test_io.ConfigVerboseLevelOnError = ImGuiTestVerboseLevel_Debug;
    test_io.ConfigSavedSettings = false;
    // Recording: each frame read back from the window (imgui_app's capture function) and piped to
    // ffmpeg, encoded as H.264.
    test_io.ConfigCaptureEnabled = record;
    if (record) {
        test_io.ScreenCaptureFunc = ImGuiApp_ScreenCaptureFunc;
        test_io.ScreenCaptureUserData = app;
        ImStrncpy(test_io.VideoCaptureEncoderPath, ffmpeg.c_str(), sizeof(test_io.VideoCaptureEncoderPath));
        ImStrncpy(test_io.VideoCaptureEncoderParams, encoder->c_str(), sizeof(test_io.VideoCaptureEncoderParams));
    }
    // Watched, the mouse the Test Engine moves is drawn, so the clicks can be followed.
    test_io.ConfigMouseDrawCursor = watch;

    ImGuiTestEngine_Start(engine, ImGui::GetCurrentContext());
    ImGuiTest* test = IM_REGISTER_TEST(engine, "demo", g_time_shift_mode ? "time_shifting"
                                                       : g_live_mode     ? "live_trading"
                                                                         : "a_market_maker_provides_liquidity");
    test->SetVarsDataType<DemoVars>(set_up);
    test->GuiFunc = viewer_frame;
    test->TestFunc = g_time_shift_mode ? play_time_shift : g_live_mode ? play_live : play_the_lesson;
    ImGuiTestEngine_QueueTests(engine, ImGuiTestGroup_Tests, "all", ImGuiTestRunFlags_RunFromCommandLine);

    while (!ImGuiTestEngine_IsTestQueueEmpty(engine)) {
        if (!app->NewFrame(app)) {
            break;  // the window was closed
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
    ImGuiTestEngine_Stop(engine);
    app->ShutdownBackends(app);
    app->ShutdownCloseWindow(app);
    ImGui::DestroyContext();
    ImGuiTestEngine_DestroyContext(engine);
    app->Destroy(app);

    g_live.client.reset();  // killed, before their directory goes
    g_live.exchange.reset();
    std::error_code ignored;
    std::filesystem::remove_all(scratch_root(), ignored);
    bool passed = summary.CountTested == 1 && summary.CountSuccess == 1;
    std::printf("viewer demo: %s\n", passed ? "played, every check passed" : "FAILED");
    if (record) {
        const bool written = std::filesystem::exists(*g_demo.record) && std::filesystem::file_size(*g_demo.record) > 0;
        std::printf("viewer demo: %s %s\n", written ? "recorded to" : "NOT recorded to", g_demo.record->c_str());
        passed = passed && written;
    }
    return passed ? 0 : 1;
}
