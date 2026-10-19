// The viewer: receives frames and draws them with Dear ImGui.
//
//   viewer [--exit-after SECONDS] [--socket NAME]
//
// It prints one line per second, "viewer: t=… paints=… frames=…", so a script can check that it
// keeps painting while the frame source is stopped in a debugger.

#include "channel/net.h"
#include "viewer/viewer_view.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <string_view>

int main(int argc, char** argv) {
    double exit_after = 0;  // 0: run until the window is closed
    std::string socket_name{fillscope::kDefaultSocketName};
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--exit-after" && i + 1 < argc) {
            exit_after = std::stod(argv[++i]);
        } else if (arg == "--socket" && i + 1 < argc) {
            socket_name = argv[++i];
        } else {
            std::fprintf(stderr, "usage: viewer [--exit-after SECONDS] [--socket NAME]\n");
            return 2;
        }
    }

    fillscope::Fd listener = fillscope::listen_on(socket_name);
    if (!listener) {
        std::fprintf(stderr, "viewer: cannot listen on %s (another viewer running?)\n", socket_name.c_str());
        return 1;
    }
    fillscope::FrameReceiver receiver{std::move(listener)};

    if (!glfwInit()) {
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow* window = glfwCreateWindow(900, 500, "Fillscope viewer", nullptr, nullptr);
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
    std::vector<fillscope::Frame> received;
    const auto start = std::chrono::steady_clock::now();
    auto last_frame_at = start;
    int last_report_second = 0;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        received.clear();
        if (receiver.drain(received) > 0) {
            last_frame_at = std::chrono::steady_clock::now();
        }
        for (const auto& frame : received) {
            state.append(frame);
        }
        state.source_connected = receiver.connected();
        const auto now = std::chrono::steady_clock::now();
        const double t = std::chrono::duration<double>(now - start).count();
        state.seconds_since_frame = std::chrono::duration<double>(now - last_frame_at).count();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        fillscope::draw_viewer(state);
        ImGui::Render();

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
            std::printf("viewer: t=%d paints=%llu frames=%zu\n", last_report_second,
                        static_cast<unsigned long long>(state.paints), state.frames.size());
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
