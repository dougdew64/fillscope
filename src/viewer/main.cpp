// The viewer: rebuilds the exchange's order books from its multicast market data, or from a
// replay's market data file, draws them with Dear ImGui, and publishes what the user points at and
// follows for the assistant working beside it.
//
//   viewer [--exit-after SECONDS] [--incremental GROUP:PORT] [--snapshot GROUP:PORT] [--replay FILE]
//          [--context-file PATH]
//
// It prints one line per second, "viewer: t=… paints=… updates=… books as of update …", so a script
// can check that it keeps painting and taking in market data, and a line on stderr whenever
// publishing the context starts failing or works again. Closing the window, --exit-after, SIGINT
// and SIGTERM all end it the same way: the last context file says it is no longer running. It
// exits 1 if that last write fails.

#include "feed/feed_socket.h"
#include "feed/market_data_file.h"
#include "feed/kept_file.h"
#include "viewer/crash_log.h"
#include "viewer/kept_files.h"
#include "viewer/log_events.h"
#include "viewer/viewer_app.h"
#include "viewer/viewer_log.h"
#include "viewer/stop_signals.h"
#include "viewer/viewer_options.h"
#include "viewer/window_size.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <algorithm>
#include <chrono>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <csignal>
#include <unistd.h>

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

// The run's log (viewer_log.h), if it could be opened: lines for it, and what the viewer says on
// stderr, which is written there too. Global, for GLFW's error callback, which can't capture.
std::optional<fillscope::ViewerLog> g_log;

void note(std::string_view line) {
    if (g_log) {
        (void)g_log->line(line);
    }
}

void say(std::string_view line) {
    note(line);  // first: whoever sees the message finds it in the log already
    std::println(stderr, "viewer: {}", line);
}

// How the run ended, and its exit code, as the log's last line.
int end_with(int code, std::string_view why) {
    note(std::format("end: {}; exit code {}", why, code));
    return code;
}

// What the build is, for the log's first lines: the compiler, optimized or not, the sanitizers.
std::string build_description() {
    std::string build = std::format("GCC {}", __VERSION__);
#if defined(__OPTIMIZE__)
    build += ", optimized";
#else
    build += ", unoptimized";
#endif
#if defined(__SANITIZE_ADDRESS__)
    build += ", AddressSanitizer and UndefinedBehaviorSanitizer";
#else
    build += ", no sanitizers";
#endif
    return build;
}

// The command line, as typed: each argument quoted if it has a space.
std::string command_line(std::span<const char* const> args) {
    std::string line = "viewer";
    for (const char* arg : args) {
        const std::string_view text{arg};
        line += text.find(' ') == std::string_view::npos ? std::format(" {}", text) : std::format(" '{}'", text);
    }
    return line;
}

// Opens the run's log in the logs directory beside the context file, and records how the viewer was
// started and that crashes are recorded. A log that can't be opened is said on stderr, and the
// viewer runs without one: it is for diagnosis, not needed to view.
void open_log(const std::filesystem::path& context_file, std::span<const char* const> args) {
    const std::filesystem::path directory = (context_file.has_parent_path() ? context_file.parent_path() : ".") / "logs";
    auto opened = fillscope::ViewerLog::open(directory, std::chrono::system_clock::now(), ::getpid());
    if (!opened) {
        std::println(stderr, "viewer: running without a log: {}", opened.error());
        return;
    }
    g_log.emplace(std::move(*opened));
    std::error_code ignored;
    note(std::format("start: {}", command_line(args)));
    note(std::format("build: {}", build_description()));
    note(std::format("process {}, in {}", ::getpid(), std::filesystem::current_path(ignored).string()));
    // The binary that ran, and when it was built: a crash's backtrace gives the viewer's frames as
    // addresses in it, which `addr2line -f -C -p -e BINARY ADDRESS...` names, while it is unchanged.
    const std::filesystem::path binary = std::filesystem::read_symlink("/proc/self/exe", ignored);
    const auto built = std::filesystem::last_write_time(binary, ignored);
    note(std::format("binary: {}, built {:%Y-%m-%dT%H:%M:%S}Z", binary.string(),
                     std::chrono::floor<std::chrono::seconds>(std::chrono::clock_cast<std::chrono::system_clock>(built))));
    note(std::format("log: {}; the newest {} kept in {}; this one stops at {} bytes", g_log->path().filename().string(),
                     fillscope::ViewerLog::kKeptRuns, directory.string(), fillscope::ViewerLog::kMaxBytes));
    note(fillscope::install_crash_log(g_log->fd(), g_log->path().string() + ".sanitizer"));
}

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
        fillscope::apply_viewer_look();
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

// Writes the frame just drawn, `width` by `height` pixels, as a binary PPM (P6): the simplest image
// format there is, which any image tool reads. OpenGL's rows run bottom to top, a PPM's top to
// bottom, so the rows are written in reverse.
std::expected<void, std::string> write_screenshot(const std::filesystem::path& file, int width, int height) {
    std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
    std::ofstream out{file, std::ios::binary};
    out << "P6\n" << width << ' ' << height << "\n255\n";
    const std::size_t row = static_cast<std::size_t>(width) * 3;
    for (int y = height - 1; y >= 0; --y) {
        out.write(reinterpret_cast<const char*>(pixels.data() + static_cast<std::size_t>(y) * row), static_cast<std::streamsize>(row));
    }
    out.close();
    if (!out) {
        return std::unexpected{std::format("can't write the screenshot {}", file.string())};
    }
    return {};
}

}  // namespace

int main(int argc, char** argv) {
    // argv's elements after the program's name; empty if there is no name (argc 0, which Linux has
    // ruled out since 5.18, but the range must be valid either way).
    const auto args = argc > 0 ? std::span<const char* const>{argv + 1, argv + argc} : std::span<const char* const>{};
    const auto options = fillscope::parse_viewer_args(args);
    if (!options) {
        std::println(stderr, "viewer: {}", options.error());
        std::println(stderr, "{}", fillscope::kViewerUsage);
        return 2;  // before the log: where it goes is part of what couldn't be read
    }
    open_log(options->context_file, args);

    // A replay's market data, or the exchange's two live streams. Joining can fail (a port bound by a
    // program that doesn't share it, say); then there is nothing to show, so the viewer says why and
    // stops, as it does for a replay it can't read.
    std::optional<fillscope::MarketDataFile> replay;
    std::optional<fillscope::FeedSocket> incremental;
    std::optional<fillscope::FeedSocket> snapshot;
    if (options->answer) {
        // Nothing to join or read yet: the answer file is watched, and read when it appears.
        note(std::format("answers: watching {}", options->answer->string()));
    } else if (options->replay) {
        auto read = fillscope::read_market_data_file(*options->replay);
        if (!read) {
            say(read.error());
            return end_with(1, "the replay can't be read");
        }
        note(std::format("replay: {}, {} updates, recorded in {}", options->replay->string(), read->updates.size(), read->recording));
        replay = std::move(*read);
    } else {
        for (const auto& [stream, socket] :
             {std::pair{&options->incremental, &incremental}, std::pair{&options->snapshot, &snapshot}}) {
            auto joined = fillscope::FeedSocket::join(*stream);
            if (!joined) {
                say(std::format("can't join the market data stream {}:{}: {}", stream->group, stream->port,
                                joined.error().message()));
                return end_with(1, "a market data stream can't be joined");
            }
            socket->emplace(std::move(*joined));
        }
        note(std::format("live: joined the incremental updates at {}:{} and the snapshots at {}:{}", options->incremental.group,
                         options->incremental.port, options->snapshot.group, options->snapshot.port));
    }
    if (const auto installed = fillscope::install_stop_signals(); !installed) {
        say(std::format("can't handle SIGINT and SIGTERM: {}", installed.error().message()));
        return end_with(1, "the stop signals can't be handled");
    }

    // GLFW reports errors through a callback; a non-capturing lambda converts to the function
    // pointer it takes.
    glfwSetErrorCallback([](int code, const char* description) { say(std::format("GLFW error {}: {}", code, description)); });
    const GlfwSession glfw;
    if (!glfw.initialized()) {
        say("can't initialize GLFW (is there a display?)");
        return end_with(1, "no display");
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    const auto [window_width, window_height] = fillscope::primary_window_size();
    const WindowPtr window{glfwCreateWindow(window_width, window_height, "Fillscope viewer", nullptr, nullptr), &glfwDestroyWindow};
    if (!window) {
        say("can't create a window with OpenGL 3.3");
        return end_with(1, "no window");
    }
    glfwMakeContextCurrent(window.get());
    // No vsync: the loop paces itself (below). With vsync, a swap waits for the compositor, which
    // sends a window no frames while it can't be seen (behind another, on another workspace); the
    // loop then stopped, and with it the reading of market data (whose buffers overflowed, losing
    // updates) and the check for SIGINT and SIGTERM.
    glfwSwapInterval(0);
    const ImGuiSession imgui{window.get()};
    if (!imgui.ready()) {
        say("can't start Dear ImGui's GLFW and OpenGL 3 backends");
        return end_with(1, "no Dear ImGui backends");
    }

    fillscope::ViewerApp app{options->context_file};
    // Live: what is kept, written to a file of this run's own in the kept directory beside the context
    // file, deleted on every way main returns; those of viewers no longer running removed first.
    std::optional<fillscope::DeleteOnExit> delete_kept_file;
    if (incremental) {
        const std::filesystem::path directory =
            (options->context_file.has_parent_path() ? options->context_file.parent_path() : ".") / "kept";
        std::error_code made;
        std::filesystem::create_directories(directory, made);
        for (const std::filesystem::path& removed : fillscope::remove_abandoned_kept_files(directory)) {
            note(std::format("kept: removed {}, left by a viewer no longer running", removed.string()));
        }
        const std::filesystem::path file = directory / fillscope::kept_file_name(::getpid());
        if (auto writer = fillscope::KeptFileWriter::create(file); writer) {
            delete_kept_file.emplace(file);
            app.keep_to(std::move(*writer));
            note(std::format("kept: writing what is kept to {}, deleted on exit", file.string()));
        } else {
            say(std::format("live moments will have no address: {}", writer.error()));
        }
    }
    if (replay) {
        app.open_replay(*options->replay, std::move(*replay));
    }
    if (options->answer) {
        app.watch_answer(*options->answer);
    }
    if (options->course) {
        auto course = fillscope::read_course_file(*options->course / "course.json");
        if (!course) {
            say(std::format("{} (the course is built by: cmake --build build/debug --target course)", course.error()));
            return end_with(1, "the course can't be read");
        }
        app.open_course(std::move(*course), *options->course);
    }
    std::optional<std::size_t> answer_frame = options->answer_frame;
    const auto start = std::chrono::steady_clock::now();
    int last_report_second = 0;
    fillscope::LogWatch log_watch;
    std::string ended_because = "the window was closed";  // unless the loop ends another way

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
        app.poll_answer(now);
        if (answer_frame && app.state().answer && app.state().answer->answer) {
            app.answer_to(*answer_frame);  // once, as soon as the answer has been read
            answer_frame.reset();
        }
        for (const std::string& event : fillscope::log_events(app.state(), log_watch)) {
            note(event);
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        if (const auto did = fillscope::describe(app.paint(now))) {
            note(*did);
        }
        ImGui::Render();
        if (const auto report = app.publish_report()) {
            say(*report);
        }
        if (const auto report = app.kept_file_report()) {
            say(*report);
        }

        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window.get(), &width, &height);
        glViewport(0, 0, width, height);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        const double t = std::chrono::duration<double>(now - start).count();
        const bool last_paint = options->exit_after_seconds > 0 && t >= options->exit_after_seconds;
        if (last_paint && options->screenshot) {
            // Read before the swap: the frame just drawn is in the back buffer.
            if (const auto written = write_screenshot(*options->screenshot, width, height); !written) {
                say(written.error());
                return end_with(1, "the screenshot can't be written");
            }
            note(std::format("screenshot: {}, {} by {}", options->screenshot->string(), width, height));
        }
        glfwSwapBuffers(window.get());

        if (static_cast<int>(t) > last_report_second) {
            last_report_second = static_cast<int>(t);
            const std::string report = std::format("t={} paints={} updates={} books as of update {}", last_report_second,
                                                   app.paints(), app.state().books.updates_received(), app.state().books.last_seq());
            note(report);  // first: whoever sees the line on stdout finds it in the log already
            std::println("viewer: {}", report);
            std::fflush(stdout);
        }
        if (last_paint) {
            ended_because = std::format("--exit-after {} s", options->exit_after_seconds);
            break;
        }
    }
    if (fillscope::stop_requested()) {
        std::println(stderr, "viewer: stopping, as a signal asked");
        ended_because = fillscope::stop_signal() == SIGINT ? "SIGINT" : "SIGTERM";
    }
    note(std::format("stopping: {}", ended_because));
    // Takes in whatever arrived since the last paint, so the last file counts every update received.
    if (incremental) {
        app.receive(*incremental, *snapshot, std::chrono::steady_clock::now());
    }
    if (!app.finish()) {  // the last file says this viewer is no longer running
        say(std::format("can't publish the final context to {}: {}", app.publisher().file().string(), *app.publisher().error()));
        return end_with(1, std::format("{}; the final context can't be published", ended_because));
    }
    note(std::format("the final context is published to {}, saying this viewer has stopped", app.publisher().file().string()));
    return end_with(0, ended_because);
}
