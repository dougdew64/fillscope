# Fillscope. How to build, run and test: docs/RUNNING.md.
#
#   -DFILLSCOPE_SANITIZE=ON   AddressSanitizer and UndefinedBehaviorSanitizer on everything
#   -DFILLSCOPE_TESTS=OFF     skip the tests and their dependencies
#   -DFILLSCOPE_WERROR=OFF    warnings in our own code don't fail the build (the checks that build
#                             deliberately broken code set this; every other build fails on one)
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
option(FILLSCOPE_WERROR "Treat warnings in our own code as errors" ON)

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

# src/feed: the exchange's market data: decoding and receiving it, and the books it describes.
add_library(fillscope_feed STATIC
  src/feed/market_update.cpp src/feed/book.cpp src/feed/book_builder.cpp src/feed/feed_socket.cpp
  src/feed/market_data_file.cpp)
target_include_directories(fillscope_feed PUBLIC src)
target_compile_options(fillscope_feed PRIVATE ${FILLSCOPE_WARNINGS})

# src/viewer: the Dear ImGui viewer.
# The helpers the viewer and the answer tool share: the file a reader never sees half of.
add_library(fillscope_common STATIC src/common/atomic_file.cpp)
target_include_directories(fillscope_common PUBLIC src)
target_compile_options(fillscope_common PRIVATE ${FILLSCOPE_WARNINGS})
add_library(imgui_app STATIC ${IMGUI_SOURCES}
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui_app PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui_app PUBLIC glfw OpenGL::GL)

# The viewer's core, without Dear ImGui: what it has received, what the user has pointed at and
# followed, and the context it publishes. Unit-tested on its own.
# The answer tool (src/answer/): plays an answer's scenario through the back end's exchange_replay,
# checks its claims, and writes the answer for the viewer.
add_library(fillscope_answer STATIC
  src/answer/scenario.cpp src/answer/wire.cpp src/answer/check.cpp src/answer/answer.cpp src/answer/course.cpp)
target_include_directories(fillscope_answer PUBLIC src)
target_link_libraries(fillscope_answer PUBLIC fillscope_common fillscope_feed nlohmann_json::nlohmann_json)
target_compile_options(fillscope_answer PRIVATE ${FILLSCOPE_WARNINGS})
add_executable(fillscope-answer src/answer/main.cpp)
target_link_libraries(fillscope-answer PRIVATE fillscope_answer)
target_compile_options(fillscope-answer PRIVATE ${FILLSCOPE_WARNINGS})
target_compile_definitions(fillscope-answer PRIVATE FILLSCOPE_EXCHANGE_REPLAY="$<TARGET_FILE:exchange_replay>")
add_dependencies(fillscope-answer exchange_replay)

# The course (course/): each lesson played through the answer tool into the build's course/, where
# the viewer offers them (viewer --course). Not built by default: every rebuild of the tool or the
# back end would replay every lesson, which the mutation check, rebuilding hundreds of times, can't
# afford. The viewer's launch builds it (the "build the course" task), and course_system_tests plays
# every lesson. One replay at a time (a job pool): each builds a matching engine, about 800 MB.
set(COURSE_DIR ${PROJECT_SOURCE_DIR}/course)
file(READ ${COURSE_DIR}/course.json course_manifest)
configure_file(${COURSE_DIR}/course.json ${PROJECT_BINARY_DIR}/course/course.json COPYONLY)
set_property(GLOBAL APPEND PROPERTY JOB_POOLS replay=1)
string(JSON lesson_count LENGTH "${course_manifest}" lessons)
math(EXPR last_lesson "${lesson_count} - 1")
set(course_answers)
foreach(i RANGE ${last_lesson})
  string(JSON scenario GET "${course_manifest}" lessons ${i} scenario)
  get_filename_component(stem ${scenario} NAME_WE)
  set(answer ${PROJECT_BINARY_DIR}/course/${stem}/answer.json)
  add_custom_command(OUTPUT ${answer}
    COMMAND $<TARGET_FILE:fillscope-answer> ${COURSE_DIR}/${scenario} --answer ${answer}
    DEPENDS fillscope-answer exchange_replay ${COURSE_DIR}/${scenario}
    JOB_POOL replay
    COMMENT "Playing the lesson ${scenario}")
  list(APPEND course_answers ${answer})
endforeach()
add_custom_target(course DEPENDS ${course_answers})

add_library(fillscope_viewer_core STATIC
  src/viewer/context.cpp src/viewer/context_publisher.cpp src/viewer/viewer_options.cpp
  src/viewer/stop_signals.cpp src/viewer/answer_state.cpp src/viewer/viewer_log.cpp src/viewer/crash_log.cpp
  src/viewer/log_events.cpp)
target_include_directories(fillscope_viewer_core PUBLIC src)
target_link_libraries(fillscope_viewer_core PUBLIC fillscope_answer fillscope_common fillscope_feed nlohmann_json::nlohmann_json)
target_compile_options(fillscope_viewer_core PRIVATE ${FILLSCOPE_WARNINGS})

# The parts that draw with Dear ImGui are compiled into each program that uses them (the viewer, and
# its UI tests, which use a Dear ImGui built with the Test Engine's hooks).
set(VIEWER_UI_SOURCES ${PROJECT_SOURCE_DIR}/src/viewer/viewer_view.cpp ${PROJECT_SOURCE_DIR}/src/viewer/viewer_app.cpp)
add_executable(viewer src/viewer/main.cpp ${VIEWER_UI_SOURCES})
target_link_libraries(viewer PRIVATE fillscope_viewer_core imgui_app)
target_compile_options(viewer PRIVATE ${FILLSCOPE_WARNINGS})

# backend: the trading system Fillscope observes, a fork of the one in Ghosh's book, pinned as a git
# submodule (docs/PLAN.md). Built with its own build file and flags, as its own build would, plus
# the sanitizers in the sanitizer build. Its programs: exchange_main and trading_main.
if(NOT EXISTS ${PROJECT_SOURCE_DIR}/backend/Chapter12/CMakeLists.txt)
  message(FATAL_ERROR "backend/ is empty: run `git submodule update --init`")
endif()
if(FILLSCOPE_TESTS)
  # GoogleTest, fetched here, before the back end, whose tests use it too (its tests/CMakeLists.txt
  # fetches the same release when it is built on its own). Its own flags would otherwise apply.
  FetchContent_Declare(googletest URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
    URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  set(LOWLATENCY_TESTS ON CACHE BOOL "Build the back end's tests" FORCE)
endif()
add_subdirectory(backend/Chapter12)


if(FILLSCOPE_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
