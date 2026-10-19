# The debugger-lockstep experiment (docs/debugger-lockstep.md): an engine process and a Dear ImGui
# viewer process. Throwaway: it answers the experiment's questions, then the real design uses them.
cmake_minimum_required(VERSION 3.24)
project(lockstep LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)  # the code under study is stepped, so -O0 by default
endif()

include(FetchContent)
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.93.0)
FetchContent_MakeAvailable(imgui)

find_package(glfw3 3.3 REQUIRED)   # Fedora: glfw-devel
find_package(OpenGL REQUIRED)      # Fedora: mesa-libGL-devel

add_compile_options(-Wall -Wextra)

add_executable(engine engine.cpp)

add_executable(viewer
  viewer.cpp
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(viewer PRIVATE ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(viewer PRIVATE glfw OpenGL::GL)
