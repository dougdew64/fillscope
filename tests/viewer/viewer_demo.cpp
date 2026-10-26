// The demo: lesson 6 of the course, "How a market maker provides liquidity", played in the viewer
// as a person would play it: chosen from the drop-down in the answer pane, then stepped through
// with > to its end. At every frame it checks what is on screen against the lesson's answer, read
// on its own (load_answer, every claim checked again): where the pane says the user is, the step's
// question or its explanation, every claim marked true, and the book, one row per order resting
// after the step, rebuilt here from the market data rather than taken from the viewer.
//
// The lesson is played first, through the real answer tool and exchange_replay, into a scratch
// directory, so the demo doesn't depend on the course having been built.
//
//   viewer_demo FILLSCOPE_ANSWER COURSE_DIR [--watch | --record FILE.mp4]
//
// In the tests, headless (the Test Engine's null backend) and at full speed. With --watch, in a
// window, at a pace a person can follow: the mouse moves to each control before it clicks, and each
// frame stays on screen for a few seconds. With --record, the same, and recorded as a video: the
// Test Engine reads each frame back from the window and pipes it to ffmpeg, so the video holds the
// window alone, its drawn mouse included. The run is the same every way, checks included. Exits 0
// only if the demo ran, every check passed, and any recording was written.

#include "answer/course.h"
#include "feed/book.h"
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

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

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

struct DemoVars {
    std::unique_ptr<fillscope::ViewerApp> app;
    std::string text;  // the text the viewer rendered last frame
};

// The viewer as `viewer --answer FILE --course DIR` starts it: the user's questions' file (empty
// here), and the course beside it, nothing chosen.
void set_up(ImGuiTestContext*, DemoVars& vars) {
    vars.app = std::make_unique<fillscope::ViewerApp>(scratch_root() / "context.json");
    vars.app->watch_answer(scratch_root() / ".fillscope" / "answer.json");
    vars.app->open_course(g_demo.course, g_demo.built);
}

// One frame of the viewer's loop, as in src/viewer/main.cpp, with the rendered text captured.
void viewer_frame(ImGuiTestContext* ctx) {
    DemoVars& vars = ctx->GetVars<DemoVars>();
    const auto now = std::chrono::steady_clock::now();
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
// takes a bit rate rather than a quality. The frames come in as the Test Engine sends them.
std::optional<std::string> h264_arguments(const std::string& ffmpeg) {
    const std::string encoders = fillscope::test::run_command(fillscope::test::shell_quote(ffmpeg) + " -hide_banner -encoders").output;
    const std::string input = "-hide_banner -loglevel error -r $FPS -f rawvideo -pix_fmt rgba -s $WIDTHx$HEIGHT -i - -threads 0 -y ";
    const std::string output = " -pix_fmt yuv420p -movflags +faststart $OUTPUT";
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
    const bool record = argc == 5 && std::string_view{argv[3]} == "--record";
    const bool watch = record || (argc == 4 && std::string_view{argv[3]} == "--watch");
    if (argc != 3 && !watch) {
        std::fprintf(stderr, "usage: viewer_demo FILLSCOPE_ANSWER COURSE_DIR [--watch | --record FILE.mp4]\n");
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
    if (!prepare(argv[1], argv[2])) {
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
    if (!app->InitCreateWindow(app, "Fillscope demo: a market maker", ImVec2(static_cast<float>(size.width), static_cast<float>(size.height)))) {
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
    ImGuiTest* test = IM_REGISTER_TEST(engine, "demo", "a_market_maker_provides_liquidity");
    test->SetVarsDataType<DemoVars>(set_up);
    test->GuiFunc = viewer_frame;
    test->TestFunc = play_the_lesson;
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
