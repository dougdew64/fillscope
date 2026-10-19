# Fillscope. How to build, run and test: docs/RUNNING.md.
#
#   -DFILLSCOPE_SANITIZE=ON   AddressSanitizer and UndefinedBehaviorSanitizer on everything
#   -DFILLSCOPE_TESTS=OFF     skip the tests and their dependencies
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

if(FILLSCOPE_SANITIZE)
  # Applied before any target, so the dependencies are instrumented too. UB stops the program
  # rather than printing and continuing, so a test can't pass over it.
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

include(FetchContent)
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.93.0)
FetchContent_MakeAvailable(imgui)

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

# src/channel: the socket that carries frames to the viewer.
add_library(fillscope_channel STATIC src/channel/net.cpp)
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

add_executable(viewer src/viewer/main.cpp src/viewer/viewer_view.cpp)
target_link_libraries(viewer PRIVATE fillscope_channel imgui_app)
target_compile_options(viewer PRIVATE ${FILLSCOPE_WARNINGS})

if(FILLSCOPE_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
