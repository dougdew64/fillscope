// The viewer: receives messages from a frame source, draws them with Dear ImGui, and publishes what
// the user points at and follows for the assistant working beside it.
//
//   viewer [--exit-after SECONDS] [--socket NAME] [--context-file PATH]
//
// It prints one line per second, "viewer: t=… paints=… frames=…", so a script can check that it
// keeps painting while the frame source is stopped in a debugger, and a line on stderr whenever
// publishing the context starts failing or works again. Closing the window, --exit-after, SIGINT
// and SIGTERM all end it the same way: the last context file says it is no longer running. It
// exits 1 if that last write fails.

#include "feed/feed_socket.h"
#include "feed/market_data_file.h"
#include "viewer/viewer_app.h"
#include "viewer/stop_signals.h"
#include "viewer/viewer_options.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <print>
#include <span>
#include <thread>

namespace {

// GLFW, initialized for the lifetime of this object and terminated by its destructor, on every path
// out of main. (RAII for a C library: the destructor is the cleanup that can't be forgotten.)
class GlfwSession {
public:
    GlfwSession() : initialized_{glfwInit() == GLFW_TRUE} {}
    GlfwSession(const GlfwSession&) = delete;
    GlfwSession& operator=(const GlfwSession&) = delete;
    ~GlfwSession() {
        if (initialized_) {
            glfwTerminate();
        }
    }
    [[nodiscard]] bool initialized() const noexcept { return initialized_; }

private:
    bool initialized_;
};

// A GLFW window, destroyed by glfwDestroyWindow when the unique_ptr goes out of scope. unique_ptr's
// second parameter is the deleter's type: here a function pointer, called instead of delete.
using WindowPtr = std::unique_ptr<GLFWwindow, decltype(&glfwDestroyWindow)>;

// Dear ImGui's context and its two backends, set up together and shut down in reverse order; only
// what was set up is shut down.
class ImGuiSession {
public:
    explicit ImGuiSession(GLFWwindow* window) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;  // no imgui.ini written to the working directory
        glfw_ready_ = ImGui_ImplGlfw_InitForOpenGL(window, true);
        opengl_ready_ = glfw_ready_ && ImGui_ImplOpenGL3_Init("#version 330");
    }
    ImGuiSession(const ImGuiSession&) = delete;
    ImGuiSession& operator=(const ImGuiSession&) = delete;
    ~ImGuiSession() {
        if (opengl_ready_) {
            ImGui_ImplOpenGL3_Shutdown();
        }
        if (glfw_ready_) {
            ImGui_ImplGlfw_Shutdown();
        }
        ImGui::DestroyContext();
    }
    [[nodiscard]] bool ready() const noexcept { return opengl_ready_; }

private:
    bool glfw_ready_ = false;
    bool opengl_ready_ = false;
};

}  // namespace

int main(int argc, char** argv) {
    // argv's elements after the program's name; empty if there is no name (argc 0, which Linux has
    // ruled out since 5.18, but the range must be valid either way).
    const auto args = argc > 0 ? std::span<const char* const>{argv + 1, argv + argc} : std::span<const char* const>{};
    const auto options = fillscope::parse_viewer_args(args);
    if (!options) {
        std::println(stderr, "viewer: {}", options.error());
        std::println(stderr, "{}", fillscope::kViewerUsage);
        return 2;
    }

    // A replay's market data, or the exchange's two live streams. Joining can fail (a port bound by a
    // program that doesn't share it, say); then there is nothing to show, so the viewer says why and
    // stops, as it does for a replay it can't read.
    std::optional<fillscope::MarketDataFile> replay;
    std::optional<fillscope::FeedSocket> incremental;
    std::optional<fillscope::FeedSocket> snapshot;
    if (options->replay) {
        auto read = fillscope::read_market_data_file(*options->replay);
        if (!read) {
            std::println(stderr, "viewer: {}", read.error());
            return 1;
        }
        replay = std::move(*read);
    } else {
        for (const auto& [stream, socket] :
             {std::pair{&options->incremental, &incremental}, std::pair{&options->snapshot, &snapshot}}) {
            auto joined = fillscope::FeedSocket::join(*stream);
            if (!joined) {
                std::println(stderr, "viewer: can't join the market data stream {}:{}: {}", stream->group, stream->port,
                             joined.error().message());
                return 1;
            }
            socket->emplace(std::move(*joined));
        }
    }
    if (const auto installed = fillscope::install_stop_signals(); !installed) {
        std::println(stderr, "viewer: can't handle SIGINT and SIGTERM: {}", installed.error().message());
        return 1;
    }

    // GLFW reports errors through a callback; a non-capturing lambda converts to the function
    // pointer it takes.
    glfwSetErrorCallback([](int code, const char* description) {
        std::println(stderr, "viewer: GLFW error {}: {}", code, description);
    });
    const GlfwSession glfw;
    if (!glfw.initialized()) {
        std::println(stderr, "viewer: can't initialize GLFW (is there a display?)");
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    const WindowPtr window{glfwCreateWindow(900, 700, "Fillscope viewer", nullptr, nullptr), &glfwDestroyWindow};
    if (!window) {
        std::println(stderr, "viewer: can't create a window with OpenGL 3.3");
        return 1;
    }
    glfwMakeContextCurrent(window.get());
    // No vsync: the loop paces itself (below). With vsync, a swap waits for the compositor, which
    // sends a window no frames while it can't be seen (behind another, on another workspace); the
    // loop then stopped, and with it the reading of market data (whose buffers overflowed, losing
    // updates) and the check for SIGINT and SIGTERM.
    glfwSwapInterval(0);
    const ImGuiSession imgui{window.get()};
    if (!imgui.ready()) {
        std::println(stderr, "viewer: can't start Dear ImGui's GLFW and OpenGL 3 backends");
        return 1;
    }

    fillscope::ViewerApp app{options->context_file};
    if (replay) {
        app.open_replay(*options->replay, std::move(*replay));
    }
    const auto start = std::chrono::steady_clock::now();
    int last_report_second = 0;

    // About 60 paints a second, whatever the display does.
    constexpr auto kPaintInterval = std::chrono::microseconds{1'000'000 / 60};
    auto next_paint = std::chrono::steady_clock::now();
    while (!glfwWindowShouldClose(window.get()) && !fillscope::stop_requested()) {
        std::this_thread::sleep_until(next_paint);
        next_paint = std::max(next_paint + kPaintInterval, std::chrono::steady_clock::now());
        glfwPollEvents();
        const auto now = std::chrono::steady_clock::now();
        if (incremental) {
            app.receive(*incremental, *snapshot, now);
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        (void)app.paint(now);
        ImGui::Render();
        if (const auto report = app.publish_report()) {
            std::println(stderr, "viewer: {}", *report);
        }

        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window.get(), &width, &height);
        glViewport(0, 0, width, height);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window.get());

        const double t = std::chrono::duration<double>(now - start).count();
        if (static_cast<int>(t) > last_report_second) {
            last_report_second = static_cast<int>(t);
            std::println("viewer: t={} paints={} updates={} books as of update {}", last_report_second, app.paints(),
                         app.state().books.updates_received(), app.state().books.last_seq());
            std::fflush(stdout);
        }
        if (options->exit_after_seconds > 0 && t >= options->exit_after_seconds) {
            break;
        }
    }
    if (fillscope::stop_requested()) {
        std::println(stderr, "viewer: stopping, as a signal asked");
    }
    // Takes in whatever arrived since the last paint, so the last file counts every update received.
    if (incremental) {
        app.receive(*incremental, *snapshot, std::chrono::steady_clock::now());
    }
    if (!app.finish()) {  // the last file says this viewer is no longer running
        std::println(stderr, "viewer: can't publish the final context to {}: {}", app.publisher().file().string(),
                     *app.publisher().error());
        return 1;
    }
    return 0;
}
