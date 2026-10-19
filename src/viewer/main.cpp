// The viewer: receives messages from a frame source, draws them with Dear ImGui, and publishes what
// the user points at and follows for the assistant working beside it.
//
//   viewer [--exit-after SECONDS] [--socket NAME] [--context-file PATH]
//
// It prints one line per second, "viewer: t=… paints=… frames=…", so a script can check that it
// keeps painting while the frame source is stopped in a debugger.

#include "channel/net.h"
#include "viewer/viewer_app.h"
#include "viewer/viewer_options.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <chrono>
#include <memory>
#include <print>
#include <span>

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

// Dear ImGui's context and its two backends, set up together and shut down in reverse order.
class ImGuiSession {
public:
    explicit ImGuiSession(GLFWwindow* window) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;  // no imgui.ini written to the working directory
        ImGui_ImplGlfw_InitForOpenGL(window, true);
        ImGui_ImplOpenGL3_Init("#version 330");
    }
    ImGuiSession(const ImGuiSession&) = delete;
    ImGuiSession& operator=(const ImGuiSession&) = delete;
    ~ImGuiSession() {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
    }
};

}  // namespace

int main(int argc, char** argv) {
    const auto options = fillscope::parse_viewer_args(std::span<const char* const>{argv + 1, argv + argc});
    if (!options) {
        std::println(stderr, "viewer: {}", options.error());
        std::println(stderr, "{}", fillscope::kViewerUsage);
        return 2;
    }

    auto listener = fillscope::listen_on(options->socket_name);
    if (!listener) {
        std::println(stderr, "viewer: can't listen on '{}': {}", options->socket_name, listener.error().message());
        return 1;
    }
    fillscope::MessageReceiver receiver{std::move(*listener)};

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
    glfwSwapInterval(1);
    const ImGuiSession imgui{window.get()};

    fillscope::ViewerApp app{options->context_file};
    const auto start = std::chrono::steady_clock::now();
    int last_report_second = 0;

    while (!glfwWindowShouldClose(window.get())) {
        glfwPollEvents();
        const auto now = std::chrono::steady_clock::now();
        app.receive(receiver, now);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        app.paint(now);
        ImGui::Render();

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
            const std::size_t frames = app.state().run ? app.state().run->frames().size() : 0;
            std::println("viewer: t={} paints={} frames={}", last_report_second, app.paints(), frames);
            std::fflush(stdout);
        }
        if (options->exit_after_seconds > 0 && t >= options->exit_after_seconds) {
            break;
        }
    }
    app.finish();  // the last file says this viewer is no longer running
    return 0;
}
