// The viewer: receives frames from the engine and draws them with Dear ImGui.
//
//   viewer [--exit-after SECONDS]
//
// It prints one line per second, "viewer: t=… paints=… frames=…", so a script can check that it
// keeps painting while the engine is stopped in a debugger.

#include "frame.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int listen_for_engine() {
    int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, lockstep::kSocketName.data(), lockstep::kSocketName.size());
    auto len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + lockstep::kSocketName.size());
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), len) != 0 || ::listen(fd, 1) != 0) {
        std::perror("viewer: bind/listen");
        return -1;
    }
    return fd;
}

}  // namespace

int main(int argc, char** argv) {
    double exit_after = 0;  // 0: run until the window is closed
    for (int i = 1; i < argc; ++i) {
        if (std::string_view{argv[i]} == "--exit-after" && i + 1 < argc) {
            exit_after = std::stod(argv[++i]);
        }
    }

    int listen_fd = listen_for_engine();
    if (listen_fd < 0) {
        return 1;
    }
    int engine_fd = -1;

    if (!glfwInit()) {
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow* window = glfwCreateWindow(900, 500, "Fillscope lockstep experiment", nullptr, nullptr);
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

    std::vector<lockstep::Frame> frames;
    std::vector<float> prices;
    std::uint64_t paints = 0;
    const auto start = std::chrono::steady_clock::now();
    auto last_frame_at = start;
    int last_report_second = 0;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // Non-blocking: accept the engine, then drain every frame it has sent.
        if (engine_fd < 0) {
            engine_fd = ::accept4(listen_fd, nullptr, nullptr, SOCK_NONBLOCK);
        }
        if (engine_fd >= 0) {
            lockstep::Frame frame;
            while (::recv(engine_fd, &frame, sizeof frame, 0) == static_cast<ssize_t>(sizeof frame)) {
                frames.push_back(frame);
                prices.push_back(static_cast<float>(frame.price));
                last_frame_at = std::chrono::steady_clock::now();
            }
        }

        const auto now = std::chrono::steady_clock::now();
        const double t = std::chrono::duration<double>(now - start).count();
        const double since_frame = std::chrono::duration<double>(now - last_frame_at).count();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("lockstep", nullptr, ImGuiWindowFlags_NoDecoration);
        ImGui::Text("engine: %s", engine_fd >= 0 ? "connected" : "waiting");
        ImGui::Text("frames received: %zu", frames.size());
        if (!frames.empty()) {
            const auto& last = frames.back();
            ImGui::Text("last frame: #%llu  price %lld  qty %u", static_cast<unsigned long long>(last.index),
                        static_cast<long long>(last.price), last.qty);
        }
        ImGui::Text("seconds since last frame: %.1f", since_frame);
        ImGui::Text("viewer paints: %llu (keeps rising while the engine is stopped)",
                    static_cast<unsigned long long>(paints));
        if (!prices.empty()) {
            ImGui::PlotLines("price", prices.data(), static_cast<int>(prices.size()), 0, nullptr, FLT_MAX,
                             FLT_MAX, ImVec2(-1, -1));
        }
        ImGui::End();
        ImGui::Render();

        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        glViewport(0, 0, width, height);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
        ++paints;

        if (static_cast<int>(t) > last_report_second) {
            last_report_second = static_cast<int>(t);
            std::printf("viewer: t=%d paints=%llu frames=%zu\n", last_report_second,
                        static_cast<unsigned long long>(paints), frames.size());
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
    if (engine_fd >= 0) {
        ::close(engine_fd);
    }
    ::close(listen_fd);
    return 0;
}
