// The viewer's UI, tested with the Dear ImGui Test Engine on its null backend: real Dear ImGui
// frames, no window and no GPU.
//
// What the viewer shows is checked two ways. Its text is captured with Dear ImGui's own logging,
// which records every piece of text actually rendered: plain Text() items have no ID, so the Test
// Engine can't query them. Items with an ID, like the price plot, are queried through the Test
// Engine.
//
//   viewer_ui_tests [FILTER]     e.g. "^viewer/shows_waiting" (Test Engine filter syntax)
//
// Exits 0 only if at least one test ran and every test that ran passed.

#include "channel/net.h"
#include "frame_source.h"
#include "support/test_support.h"
#include "viewer/viewer_view.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_te_context.h>
#include <imgui_te_engine.h>
#include <imgui_app.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>

namespace {

using fillscope::Frame;

struct UiVars {
    fillscope::ViewerState state;
    std::string text;  // the text the viewer rendered in the last frame
    std::unique_ptr<fillscope::FrameReceiver> receiver;  // set only when a real frame source sends frames
};

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// Every frame: take in any frames from a real frame source, then draw the viewer and capture its text.
void draw_and_capture(ImGuiTestContext* ctx) {
    UiVars& vars = ctx->GetVars<UiVars>();
    if (vars.receiver) {
        std::vector<Frame> received;
        vars.receiver->drain(received);
        for (const Frame& frame : received) {
            vars.state.append(frame);
        }
        vars.state.source_connected = vars.receiver->connected();
    }
    ImGui::LogToBuffer();
    fillscope::draw_viewer(vars.state);
    vars.text = ImGui::GetCurrentContext()->LogBuffer.c_str();
    ImGui::LogFinish();
}

void register_viewer_tests(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "viewer", "shows_counts_and_last_frame");
    t->SetVarsDataType<UiVars>();
    t->GuiFunc = draw_and_capture;
    t->TestFunc = [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        vars.state.source_connected = true;
        vars.state.append(Frame{0, 10'001, 5, 0});
        vars.state.append(Frame{1, 10'002, 6, 0});
        vars.state.append(Frame{2, 10'003, 7, 0});
        ctx->Yield();
        IM_CHECK(contains(vars.text, "frames received: 3"));
        IM_CHECK(contains(vars.text, "last frame: #2  price 10003  qty 7"));
    };

    t = IM_REGISTER_TEST(engine, "viewer", "shows_waiting_without_source");
    t->SetVarsDataType<UiVars>();
    t->GuiFunc = draw_and_capture;
    t->TestFunc = [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        ctx->Yield();
        IM_CHECK(contains(vars.text, "source: waiting"));
        IM_CHECK(contains(vars.text, "frames received: 0"));
        IM_CHECK(!contains(vars.text, "last frame: #"));  // not "seconds since last frame:"
        vars.state.source_connected = true;
        ctx->Yield();
        IM_CHECK(contains(vars.text, "source: connected"));
        IM_CHECK(!contains(vars.text, "source: waiting"));
    };

    t = IM_REGISTER_TEST(engine, "viewer", "plot_appears_with_frames");
    t->SetVarsDataType<UiVars>();
    t->GuiFunc = draw_and_capture;
    t->TestFunc = [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        ctx->SetRef(fillscope::kViewerWindow);
        ctx->Yield(2);
        IM_CHECK(!ctx->ItemExists(fillscope::kPricePlot));
        vars.state.append(Frame{0, 10'000, 1, 0});
        vars.state.append(Frame{1, 10'002, 1, 0});
        ctx->Yield(2);
        IM_CHECK(ctx->ItemExists(fillscope::kPricePlot));
    };

    t = IM_REGISTER_TEST(engine, "viewer", "frames_from_source_process");
    t->SetVarsDataType<UiVars>();
    t->GuiFunc = draw_and_capture;
    t->TestFunc = [](ImGuiTestContext* ctx) {
        UiVars& vars = ctx->GetVars<UiVars>();
        const std::string name = fillscope::test::unique_socket_name();
        vars.receiver = std::make_unique<fillscope::FrameReceiver>(fillscope::listen_on(name));
        constexpr std::uint64_t kFrames = 5;
        fillscope::test::ChildProcess source{
            {FILLSCOPE_FRAME_SOURCE, "--socket", name, "--frames", std::to_string(kFrames), "--interval-ms", "0"}};
        IM_CHECK(source.started());

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (vars.state.frames.size() < kFrames && std::chrono::steady_clock::now() < deadline) {
            ctx->Yield();
        }
        IM_CHECK_EQ(source.wait(), 0);
        IM_CHECK_EQ(vars.state.frames.size(), kFrames);
        fillscope::FrameGenerator expected{fillscope::SourceOptions{}.seed};
        for (const Frame& frame : vars.state.frames) {
            IM_CHECK(frame == expected.next());
        }
        ctx->Yield();
        IM_CHECK(contains(vars.text, "frames received: 5"));
        vars.receiver.reset();
    };
}

}  // namespace

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : "all";

    ImGuiApp* app = ImGuiApp_ImplNull_Create();
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    app->InitCreateWindow(app, "viewer UI tests", ImVec2(900, 500));
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

    std::printf("viewer UI tests: %d passed of %d run\n", summary.CountSuccess, summary.CountTested);
    return summary.CountTested > 0 && summary.CountSuccess == summary.CountTested ? 0 : 1;
}
