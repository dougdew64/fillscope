// The viewer's UI, tested with the Dear ImGui Test Engine on its null backend: real Dear ImGui
// frames and real clicks, with no window and no GPU.
//
// Each test runs the viewer's whole loop every frame, as src/viewer/main.cpp does: take in
// messages, draw, apply what the user did, publish the context (to a scratch file per test).
//
// What the viewer shows is checked two ways. Its text is captured with Dear ImGui's own logging,
// which records every piece of text actually rendered: plain Text() items have no ID, so the Test
// Engine can't query them. Items with an ID (rows, buttons, the plot) are found and clicked through
// the Test Engine; "**/" in a reference searches the whole window for an item with that label.
//
//   viewer_ui_tests [FILTER]     e.g. "^viewer/points_at" (Test Engine filter syntax)
//
// Exits 0 only if at least one test ran and every test that ran passed.

#include "channel/net.h"
#include "frame_source.h"
#include "support/test_support.h"
#include "viewer/context.h"
#include "viewer/context_publisher.h"
#include "viewer/viewer_state.h"
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
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <string>

namespace {

using fillscope::Frame;

// Each test's scratch files live under here; removed when the program ends.
std::filesystem::path scratch_root() {
    return std::filesystem::temp_directory_path() / std::format("fillscope-viewer-ui-{}", ::getpid());
}

struct UiVars {
    fillscope::ViewerState state;
    fillscope::Selection selection;
    std::unique_ptr<fillscope::ContextPublisher> publisher;
    std::unique_ptr<fillscope::MessageReceiver> receiver;  // set only when a real frame source sends
    std::string text;                                      // the text the viewer rendered last frame
};

// Runs before the test's first frame: a publisher writing to this test's own scratch file.
void set_up(ImGuiTestContext* ctx, UiVars& vars) {
    const std::filesystem::path dir = scratch_root() / ctx->Test->Name;
    std::filesystem::remove_all(dir);
    vars.publisher = std::make_unique<fillscope::ContextPublisher>(dir / "context.json");
}

bool contains(const std::string& text, std::string_view part) { return text.find(part) != std::string::npos; }

// The context file as it is now.
nlohmann::json read_context(const UiVars& vars) {
    std::ifstream in{vars.publisher->file()};
    return nlohmann::json::parse(in);
}

// Gives the viewer a described run (seed 42) with these frames.
void add_run(UiVars& vars, std::initializer_list<Frame> frames) {
    if (vars.state.apply(fillscope::SourceHello{.seed = 42, .frame_count = frames.size()})) {
        vars.selection.new_run();
    }
    for (const Frame& frame : frames) {
        vars.state.apply(frame);
    }
    vars.state.source_connected = true;
}

// Five frames; ids 7, 2, 7, 9, 7.
void add_five_frames(UiVars& vars) {
    add_run(vars, {Frame{.index = 0, .price = 10'001, .qty = 5, .id = 7}, Frame{.index = 1, .price = 10'002, .qty = 6, .id = 2},
                   Frame{.index = 2, .price = 10'003, .qty = 7, .id = 7}, Frame{.index = 3, .price = 10'002, .qty = 8, .id = 9},
                   Frame{.index = 4, .price = 10'004, .qty = 9, .id = 7}});
}

// One frame of the viewer's loop, as in src/viewer/main.cpp.
void viewer_frame(ImGuiTestContext* ctx) {
    UiVars& vars = ctx->GetVars<UiVars>();
    if (vars.receiver) {
        std::vector<fillscope::Message> received;
        vars.receiver->drain(received);
        for (const fillscope::Message& message : received) {
            if (vars.state.apply(message)) {
                vars.selection.new_run();
            }
        }
        vars.state.source_connected = vars.receiver->connected();
    }
    ImGui::LogToBuffer();
    const fillscope::ViewerActions actions =
        fillscope::draw_viewer(vars.state, vars.selection, vars.publisher->published(), vars.publisher->error());
    vars.text = ImGui::GetCurrentContext()->LogBuffer.c_str();
    ImGui::LogFinish();
    fillscope::apply(actions, vars.selection);
    vars.publisher->update(fillscope::make_context(vars.state, vars.selection), actions.any(),
                           std::chrono::steady_clock::now());
}

// Runs frames until `done()` is true or three seconds of real time have passed; returns done().
// For what waits on real time: a change the user didn't make (frames arriving) is published at
// most once per ContextPublisher interval of real time, and the Test Engine's fast mode runs many
// frames in that interval.
template <typename Predicate>
bool yield_until(ImGuiTestContext* ctx, Predicate done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        ctx->Yield();
    }
    return done();
}

// Registers a test that runs the viewer's loop, with `body` as its test function.
ImGuiTest* viewer_test(ImGuiTestEngine* engine, const char* name, void (*body)(ImGuiTestContext*)) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "viewer", name);
    t->SetVarsDataType<UiVars>(set_up);
    t->GuiFunc = viewer_frame;
    t->TestFunc = body;
    return t;
}

void register_viewer_tests(ImGuiTestEngine* engine) {
    viewer_test(engine, "shows_counts_and_last_frame", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        add_five_frames(vars);
        ctx->Yield();
        IM_CHECK(contains(vars.text, "frames received: 5"));
        IM_CHECK(contains(vars.text, "last frame: #4  id 7  price 10004  qty 9"));
    });

    viewer_test(engine, "shows_waiting_without_source", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        ctx->Yield();
        IM_CHECK(contains(vars.text, "source: waiting"));
        IM_CHECK(contains(vars.text, "frames received: 0"));
        IM_CHECK(!contains(vars.text, "last frame: #"));  // not "seconds since last frame:"
        vars.state.source_connected = true;
        ctx->Yield();
        IM_CHECK(contains(vars.text, "source: connected"));
        IM_CHECK(!contains(vars.text, "source: waiting"));
    });

    viewer_test(engine, "plot_and_table_appear_with_frames", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        ctx->SetRef(fillscope::kViewerWindow);
        ctx->Yield(2);
        IM_CHECK(!ctx->ItemExists(fillscope::kPricePlot));
        IM_CHECK(!ctx->ItemExists("**/0##point"));
        add_five_frames(vars);
        ctx->Yield(2);
        IM_CHECK(ctx->ItemExists(fillscope::kPricePlot));
        IM_CHECK(ctx->ItemExists("**/4##point"));
    });

    viewer_test(engine, "the_context_bar_starts_with_the_background", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        ctx->Yield();
        IM_CHECK(contains(vars.text, "Context no frames yet · not connected"));
        add_five_frames(vars);
        // Frames arriving aren't the user's doing, so the bar changes once the publisher's interval
        // has passed and the file says so, not before.
        IM_CHECK(yield_until(ctx, [&] { return contains(vars.text, "Context source seed 42 · 5 of 5 frames · connected"); }));
        IM_CHECK(!contains(vars.text, "Pointing at"));
        IM_CHECK(!contains(vars.text, "Following"));
        const nlohmann::json context = read_context(vars);
        IM_CHECK(context["point"].is_null());
        IM_CHECK(context["follow"].is_null());
        IM_CHECK_EQ(context["background"]["framesReceived"].get<int>(), 5);
    });

    viewer_test(engine, "clicking_a_row_points_at_its_frame", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        add_five_frames(vars);
        ctx->SetRef(fillscope::kViewerWindow);
        ctx->Yield(2);
        ctx->ItemClick("**/2##point");
        ctx->Yield(2);

        IM_CHECK(vars.selection.pointed().has_value() && vars.selection.pointed()->index == 2);
        IM_CHECK(contains(vars.text, "Pointing at frame #2 (id 7, price 10003, qty 7)"));
        const nlohmann::json context = read_context(vars);
        IM_CHECK_EQ(context["point"]["frame"]["index"].get<int>(), 2);
        IM_CHECK_EQ(context["point"]["replayAddress"]["seed"].get<int>(), 42);
        IM_CHECK_EQ(context["point"]["replayAddress"]["frameIndex"].get<int>(), 2);
    });

    viewer_test(engine, "clicking_an_id_follows_it", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        add_five_frames(vars);
        ctx->SetRef(fillscope::kViewerWindow);
        ctx->Yield(2);
        ctx->ItemClick("**/7##follow2");
        ctx->Yield(2);

        IM_CHECK(vars.selection.followed() == 7u);
        IM_CHECK(contains(vars.text, "Following id 7: 3 frames"));
        const nlohmann::json context = read_context(vars);
        IM_CHECK_EQ(context["follow"]["id"].get<int>(), 7);
        IM_CHECK(context["follow"]["frameIndices"] == nlohmann::json::array({0, 2, 4}));
        // Clicking the id points at nothing: the row behind the button doesn't take the click.
        IM_CHECK(context["point"].is_null());
    });

    viewer_test(engine, "the_last_action_has_the_higher_seq", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        add_five_frames(vars);
        ctx->SetRef(fillscope::kViewerWindow);
        ctx->Yield(2);
        ctx->ItemClick("**/1##point");
        ctx->ItemClick("**/9##follow3");
        ctx->Yield(2);
        nlohmann::json context = read_context(vars);
        IM_CHECK_GT(context["follow"]["seq"].get<int>(), context["point"]["seq"].get<int>());
        ctx->ItemClick("**/0##point");
        ctx->Yield(2);
        context = read_context(vars);
        IM_CHECK_GT(context["point"]["seq"].get<int>(), context["follow"]["seq"].get<int>());
    });

    viewer_test(engine, "clear_buttons_clear_each_section", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        add_five_frames(vars);
        ctx->SetRef(fillscope::kViewerWindow);
        ctx->Yield(2);
        ctx->ItemClick("**/2##point");
        ctx->ItemClick("**/2##follow1");
        ctx->Yield(2);
        IM_CHECK(contains(vars.text, "Pointing at"));
        IM_CHECK(contains(vars.text, "Following id 2"));

        ctx->ItemClick(std::format("**/{}", fillscope::kClearPointButton).c_str());
        ctx->Yield(2);
        IM_CHECK(!contains(vars.text, "Pointing at"));
        IM_CHECK(read_context(vars)["point"].is_null());
        IM_CHECK(!read_context(vars)["follow"].is_null());

        ctx->ItemClick(std::format("**/{}", fillscope::kClearFollowButton).c_str());
        ctx->Yield(2);
        IM_CHECK(!contains(vars.text, "Following"));
        IM_CHECK(read_context(vars)["follow"].is_null());
    });

    viewer_test(engine, "the_bar_says_exactly_what_the_file_holds", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        add_five_frames(vars);
        ctx->SetRef(fillscope::kViewerWindow);
        ctx->Yield(2);
        ctx->ItemClick("**/3##point");
        ctx->ItemClick("**/7##follow4");
        ctx->Yield(2);

        // The file is the published context, as JSON...
        IM_CHECK(vars.publisher->published().has_value());
        const fillscope::Context& published = *vars.publisher->published();
        const nlohmann::json file = read_context(vars);
        const auto expected = fillscope::to_json(published, vars.publisher->seq(), file["writtenAtMs"].get<std::int64_t>());
        IM_CHECK(file == nlohmann::json::parse(expected.dump()));
        // ...and every line of the bar is drawn from that same context.
        const fillscope::ContextBarText bar = fillscope::bar_text(published);
        IM_CHECK(contains(vars.text, bar.background));
        IM_CHECK(bar.point && contains(vars.text, *bar.point));
        IM_CHECK(bar.follow && contains(vars.text, *bar.follow));
    });

    viewer_test(engine, "a_failed_write_shows_in_the_bar", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        // A directory can't be created inside a regular file, so every write to this path fails.
        const std::filesystem::path blocker = scratch_root() / "not-a-directory";
        std::filesystem::create_directories(scratch_root());
        std::ofstream{blocker} << "x";
        vars.publisher = std::make_unique<fillscope::ContextPublisher>(blocker / "context.json");
        add_five_frames(vars);
        ctx->SetRef(fillscope::kViewerWindow);
        ctx->Yield(2);
        ctx->ItemClick("**/2##point");
        ctx->Yield(2);

        IM_CHECK(contains(vars.text, "NOT published: can't create"));
        // The selection changed, but nothing reached the file, so the bar mustn't claim it did.
        IM_CHECK(vars.selection.pointed().has_value());
        IM_CHECK(!contains(vars.text, "Pointing at"));
    });

    viewer_test(engine, "hovering_says_a_click_sends", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        add_five_frames(vars);
        ctx->SetRef(fillscope::kViewerWindow);
        ctx->Yield(2);
        ctx->MouseMove("**/7##follow0");
        // A tooltip appears once the mouse has been still for a moment. The Test Engine's fast mode
        // skips ordinary sleeps, so this one advances the UI's own clock frame by frame.
        ctx->SleepNoSkip(1.0f, 1.0f / 60.0f);
        IM_CHECK(contains(vars.text, "Follow id 7: sends it to the assistant now"));
    });

    viewer_test(engine, "a_real_source_process_describes_its_run_and_sends_frames", [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        const std::string name = fillscope::test::unique_socket_name();
        vars.receiver = std::make_unique<fillscope::MessageReceiver>(fillscope::listen_on(name));
        constexpr std::uint64_t kFrames = 5;
        fillscope::test::ChildProcess source{{FILLSCOPE_FRAME_SOURCE, "--socket", name, "--frames",
                                              std::to_string(kFrames), "--interval-ms", "0", "--seed", "11"}};
        IM_CHECK(source.started());

        const auto received = [&] { return vars.state.run ? vars.state.run->frames().size() : 0; };
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (received() < kFrames && std::chrono::steady_clock::now() < deadline) {
            ctx->Yield();
        }
        IM_CHECK_EQ(source.wait(), 0);
        IM_CHECK_EQ(received(), kFrames);
        IM_CHECK(vars.state.run->hello() == (fillscope::SourceHello{.seed = 11, .frame_count = kFrames}));
        fillscope::FrameGenerator expected{11};
        for (const Frame& frame : vars.state.run->frames()) {
            IM_CHECK(frame == expected.next());
        }
        IM_CHECK(yield_until(ctx, [&] { return contains(vars.text, "source seed 11 · 5 of 5 frames"); }));
        vars.receiver.reset();
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
