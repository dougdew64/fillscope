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

  FetchContent_Declare(googletest GIT_REPOSITORY https://github.com/google/googletest.git GIT_TAG v1.17.0)
  FetchContent_Declare(imgui_test_engine GIT_REPOSITORY https://github.com/ocornut/imgui_test_engine.git
    GIT_TAG v1.93.0)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest imgui_test_engine)

  include(GoogleTest)

  set(TEST_DEFINES
    FILLSCOPE_FRAME_SOURCE="$<TARGET_FILE:frame_source>"
    FILLSCOPE_LLDB_SCRIPT="${PROJECT_SOURCE_DIR}/tests/lldb_self_stop.py")

  # Every test program is registered through fillscope_test_program, which gives its tests:
  #   - a cost tier, as a label and a per-test timeout, so a slow test fails as slow the day it
  #     is added: unit (2 s), ui (10 s, the whole program), debugger (30 s);
  #   - a label naming the program, exe:<name>, so tests/iterate.py can run one program's tests;
  #   - a line in test-manifest.txt: the program, its tier, and the files it uses at run time.
  # tests/iterate.py re-runs a program only when the program or one of those files has changed.
  # A program's tests are all in one tier, so a program is either fast or slow, never both.
  set(FILLSCOPE_TIMEOUT_unit 2)
  set(FILLSCOPE_TIMEOUT_ui 10)
  set(FILLSCOPE_TIMEOUT_debugger 30)

  function(fillscope_test_program name tier)
    cmake_parse_arguments(ARG "GTEST" "" "RUNTIME_DEPS" ${ARGN})
    if(NOT DEFINED FILLSCOPE_TIMEOUT_${tier})
      message(FATAL_ERROR "${name}: unknown test tier '${tier}'")
    endif()
    set(labels "${tier}\\;exe:${name}")
    if(ARG_GTEST)
      gtest_discover_tests(${name} DISCOVERY_MODE PRE_TEST
        PROPERTIES LABELS "${labels}" TIMEOUT ${FILLSCOPE_TIMEOUT_${tier}})
    else()
      add_test(NAME ${name} COMMAND ${name})
      set_tests_properties(${name} PROPERTIES LABELS "${tier};exe:${name}" TIMEOUT ${FILLSCOPE_TIMEOUT_${tier}})
    endif()
    list(JOIN ARG_RUNTIME_DEPS " " runtime_deps)
    set_property(GLOBAL APPEND PROPERTY FILLSCOPE_TEST_MANIFEST
      "${name} ${tier} $<TARGET_FILE:${name}> ${runtime_deps}")
  endfunction()

  function(fillscope_gtest name tier)
    cmake_parse_arguments(ARG "" "" "SOURCES;LIBS;RUNTIME_DEPS" ${ARGN})
    add_executable(${name} ${ARG_SOURCES})
    target_link_libraries(${name} PRIVATE ${ARG_LIBS} GTest::gtest_main)
    target_include_directories(${name} PRIVATE tests)
    target_compile_definitions(${name} PRIVATE ${TEST_DEFINES})
    target_compile_options(${name} PRIVATE ${FILLSCOPE_WARNINGS})
    fillscope_test_program(${name} ${tier} GTEST RUNTIME_DEPS ${ARG_RUNTIME_DEPS})
  endfunction()

  fillscope_gtest(channel_tests unit SOURCES tests/channel_test.cpp LIBS fillscope_channel)
  fillscope_gtest(lockstep_tests unit SOURCES tests/lockstep_test.cpp LIBS fillscope_lockstep)
  fillscope_gtest(frame_source_tests unit SOURCES tests/frame_source_test.cpp LIBS frame_source_lib)
  fillscope_gtest(debugger_tests debugger SOURCES tests/debugger_test.cpp LIBS fillscope_channel
    RUNTIME_DEPS $<TARGET_FILE:frame_source> ${PROJECT_SOURCE_DIR}/tests/lldb_self_stop.py)
  add_dependencies(debugger_tests frame_source)

  # Dear ImGui again, built with the Test Engine's hooks (tests/imconfig_test_engine.h), plus the
  # Test Engine and its null backend. Kept apart from imgui_app so the viewer itself has no hooks.
  set(TE_DIR ${imgui_test_engine_SOURCE_DIR})
  add_library(imgui_te STATIC ${IMGUI_SOURCES}
    ${TE_DIR}/imgui_test_engine/imgui_te_context.cpp
    ${TE_DIR}/imgui_test_engine/imgui_te_coroutine.cpp
    ${TE_DIR}/imgui_test_engine/imgui_te_engine.cpp
    ${TE_DIR}/imgui_test_engine/imgui_te_exporters.cpp
    ${TE_DIR}/imgui_test_engine/imgui_te_perftool.cpp
    ${TE_DIR}/imgui_test_engine/imgui_te_ui.cpp
    ${TE_DIR}/imgui_test_engine/imgui_te_utils.cpp
    ${TE_DIR}/imgui_test_engine/imgui_capture_tool.cpp
    ${TE_DIR}/shared/imgui_app.cpp)
  target_include_directories(imgui_te PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends
    ${TE_DIR} ${TE_DIR}/imgui_test_engine ${TE_DIR}/shared ${PROJECT_SOURCE_DIR}/tests)
  target_compile_definitions(imgui_te PUBLIC IMGUI_USER_CONFIG="imconfig_test_engine.h")
  target_link_libraries(imgui_te PUBLIC Threads::Threads)

  add_executable(viewer_ui_tests tests/viewer_ui_test.cpp src/viewer/viewer_view.cpp)
  target_link_libraries(viewer_ui_tests PRIVATE frame_source_lib imgui_te)
  target_compile_definitions(viewer_ui_tests PRIVATE ${TEST_DEFINES})
  target_compile_options(viewer_ui_tests PRIVATE ${FILLSCOPE_WARNINGS})
  add_dependencies(viewer_ui_tests frame_source)
  fillscope_test_program(viewer_ui_tests ui RUNTIME_DEPS $<TARGET_FILE:frame_source>)

  # One line per test program: name, tier, program, run-time files. Read by tests/iterate.py.
  get_property(manifest GLOBAL PROPERTY FILLSCOPE_TEST_MANIFEST)
  list(JOIN manifest "\n" manifest)
  file(GENERATE OUTPUT ${PROJECT_BINARY_DIR}/test-manifest.txt CONTENT "${manifest}\n")
endif()
