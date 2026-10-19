// The viewer: receives messages from a frame source, draws them with Dear ImGui, and publishes what
// the user points at and follows for the assistant working beside it.
//
//   viewer [--exit-after SECONDS] [--socket NAME] [--context-file PATH]
//
// It prints one line per second, "viewer: t=… paints=… frames=…", so a script can check that it
// keeps painting while the frame source is stopped in a debugger. The context file defaults to
// .fillscope/context.json in the working directory (the repository, when started from VS Code).

#include "channel/net.h"
#include "viewer/context.h"
#include "viewer/context_publisher.h"
#include "viewer/viewer_state.h"
#include "viewer/viewer_view.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

int main(int argc, char** argv) {
    double exit_after = 0;  // 0: run until the window is closed
    std::string socket_name{fillscope::kDefaultSocketName};
    std::filesystem::path context_file{fillscope::kDefaultContextFile};
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        const bool has_value = i + 1 < argc;
        if (arg == "--exit-after" && has_value) {
            exit_after = std::stod(argv[++i]);
        } else if (arg == "--socket" && has_value) {
            socket_name = argv[++i];
        } else if (arg == "--context-file" && has_value) {
            context_file = argv[++i];
        } else {
            std::fprintf(stderr, "usage: viewer [--exit-after SECONDS] [--socket NAME] [--context-file PATH]\n");
            return 2;
        }
    }

    auto listener = fillscope::listen_on(socket_name);
    if (!listener) {
        std::fprintf(stderr, "viewer: can't listen on %s: %s\n", socket_name.c_str(), listener.error().message().c_str());
        return 1;
    }
    fillscope::MessageReceiver receiver{std::move(*listener)};

    if (!glfwInit()) {
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow* window = glfwCreateWindow(900, 700, "Fillscope viewer", nullptr, nullptr);
    if (window == nullptr) {
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;  // no imgui.ini written to the working directory
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    fillscope::ViewerState state;
    fillscope::Selection selection;
    fillscope::ContextPublisher publisher{context_file};
    std::vector<fillscope::Message> received;
    const auto start = std::chrono::steady_clock::now();
    auto last_frame_at = start;
    int last_report_second = 0;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // Take in everything the source has sent since the last paint.
        received.clear();
        if (receiver.drain(received).received > 0) {
            last_frame_at = std::chrono::steady_clock::now();
        }
        for (const fillscope::Message& message : received) {
            if (state.apply(message)) {
                selection.new_run();  // the frame pointed at belonged to the previous run
            }
        }
        state.source_connected = receiver.connected();
        const auto now = std::chrono::steady_clock::now();
        const double t = std::chrono::duration<double>(now - start).count();
        state.seconds_since_frame = std::chrono::duration<double>(now - last_frame_at).count();

        // Draw, and collect what the user did.
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        const fillscope::ViewerActions actions =
            fillscope::draw_viewer(state, selection, publisher.published(), publisher.error());
        ImGui::Render();

        // Apply it, and publish the context it produces: at once if the user did something.
        fillscope::apply(actions, selection);
        publisher.update(fillscope::make_context(state, selection), actions.any(), now);

        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        glViewport(0, 0, width, height);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
        ++state.paints;

        if (static_cast<int>(t) > last_report_second) {
            last_report_second = static_cast<int>(t);
            const std::size_t frames = state.run ? state.run->frames().size() : 0;
            std::printf("viewer: t=%d paints=%llu frames=%zu\n", last_report_second,
                        static_cast<unsigned long long>(state.paints), frames);
            std::fflush(stdout);
        }
        if (exit_after > 0 && t >= exit_after) {
            break;
        }
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
