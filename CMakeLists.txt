# Fillscope. How to build, run and test: docs/RUNNING.md.
#
#   -DFILLSCOPE_SANITIZE=ON   AddressSanitizer and UndefinedBehaviorSanitizer on everything
#   -DFILLSCOPE_TESTS=OFF     skip the tests and their dependencies
#   -DFILLSCOPE_WERROR=ON     warnings in our own code fail the build (CI sets this)
cmake_minimum_required(VERSION 3.24)
project(fillscope LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)  # the code under study is stepped in a debugger, so -O0 by default
endif()

option(FILLSCOPE_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(FILLSCOPE_TESTS "Build the tests" ON)
option(FILLSCOPE_WERROR "Treat warnings in our own code as errors" OFF)

if(FILLSCOPE_SANITIZE)
  # Applied before any target, so the dependencies are instrumented too. UB stops the program
  # rather than printing and continuing, so a test can't pass over it.
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

include(FetchContent)
# Each dependency is fetched as the release's archive and checked against its SHA-256, so a moved
# tag or a tampered download fails the build rather than changing what is built.
FetchContent_Declare(imgui URL https://github.com/ocornut/imgui/archive/refs/tags/v1.93.0.tar.gz
  URL_HASH SHA256=f7bf6bc76e5d79d40d751096663e6c5d18daf08129dd52e7f7b09e4251c8eeee)
FetchContent_MakeAvailable(imgui)
# nlohmann/json: the context file the viewer publishes is JSON.
FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
  URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa)
FetchContent_MakeAvailable(json)

find_package(glfw3 3.3 REQUIRED)   # Fedora: glfw-devel
find_package(OpenGL REQUIRED)      # Fedora: mesa-libGL-devel
find_package(Threads REQUIRED)

set(IMGUI_SOURCES
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp)

# Our own code gets warnings; the fetched dependencies don't.
set(FILLSCOPE_WARNINGS -Wall -Wextra)
if(FILLSCOPE_WERROR)
  list(APPEND FILLSCOPE_WARNINGS -Werror)
endif()

# src/channel: the messages a frame source sends the viewer, and the socket that carries them.
add_library(fillscope_channel STATIC src/channel/message.cpp src/channel/net.cpp)
target_include_directories(fillscope_channel PUBLIC src)
target_compile_options(fillscope_channel PRIVATE ${FILLSCOPE_WARNINGS})

# src/lockstep: the debugger anchor.
add_library(fillscope_lockstep STATIC src/lockstep/anchor.cpp)
target_include_directories(fillscope_lockstep PUBLIC src)
target_compile_options(fillscope_lockstep PRIVATE ${FILLSCOPE_WARNINGS})

# tools/frame_source: the stand-in that produces frames until real ones exist.
add_library(frame_source_lib STATIC tools/frame_source/frame_source.cpp)
target_include_directories(frame_source_lib PUBLIC tools/frame_source)
target_link_libraries(frame_source_lib PUBLIC fillscope_channel fillscope_lockstep)
target_compile_options(frame_source_lib PRIVATE ${FILLSCOPE_WARNINGS})

add_executable(frame_source tools/frame_source/main.cpp)
target_link_libraries(frame_source PRIVATE frame_source_lib)
target_compile_options(frame_source PRIVATE ${FILLSCOPE_WARNINGS})

# src/viewer: the Dear ImGui viewer.
add_library(imgui_app STATIC ${IMGUI_SOURCES}
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui_app PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui_app PUBLIC glfw OpenGL::GL)

# The viewer's core, without Dear ImGui: what it has received, what the user has pointed at and
# followed, and the context it publishes. Unit-tested on its own.
add_library(fillscope_viewer_core STATIC
  src/viewer/viewer_state.cpp src/viewer/context.cpp src/viewer/context_publisher.cpp src/viewer/viewer_options.cpp
  src/viewer/stop_signals.cpp)
target_include_directories(fillscope_viewer_core PUBLIC src)
target_link_libraries(fillscope_viewer_core PUBLIC fillscope_channel nlohmann_json::nlohmann_json)
target_compile_options(fillscope_viewer_core PRIVATE ${FILLSCOPE_WARNINGS})

# The parts that draw with Dear ImGui are compiled into each program that uses them (the viewer, and
# its UI tests, which use a Dear ImGui built with the Test Engine's hooks).
set(VIEWER_UI_SOURCES ${PROJECT_SOURCE_DIR}/src/viewer/viewer_view.cpp ${PROJECT_SOURCE_DIR}/src/viewer/viewer_app.cpp)
add_executable(viewer src/viewer/main.cpp ${VIEWER_UI_SOURCES})
target_link_libraries(viewer PRIVATE fillscope_viewer_core imgui_app)
target_compile_options(viewer PRIVATE ${FILLSCOPE_WARNINGS})

if(FILLSCOPE_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
