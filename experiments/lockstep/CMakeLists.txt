# The debugger-lockstep experiment (docs/debugger-lockstep.md): an engine process and a Dear ImGui
# viewer process, with their tests.
#
#   -DLOCKSTEP_SANITIZE=ON   AddressSanitizer and UndefinedBehaviorSanitizer on everything
#   -DLOCKSTEP_TESTS=OFF     skip the tests and their dependencies
cmake_minimum_required(VERSION 3.24)
project(lockstep LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)  # the code under study is stepped, so -O0 by default
endif()

option(LOCKSTEP_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(LOCKSTEP_TESTS "Build the tests" ON)

if(LOCKSTEP_SANITIZE)
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
set(LOCKSTEP_WARNINGS -Wall -Wextra)

add_library(lockstep_net STATIC net.cpp)
target_include_directories(lockstep_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(lockstep_net PRIVATE ${LOCKSTEP_WARNINGS})

add_library(lockstep_engine STATIC engine_lib.cpp)
target_link_libraries(lockstep_engine PUBLIC lockstep_net)
target_compile_options(lockstep_engine PRIVATE ${LOCKSTEP_WARNINGS})

add_executable(engine engine.cpp)
target_link_libraries(engine PRIVATE lockstep_engine)
target_compile_options(engine PRIVATE ${LOCKSTEP_WARNINGS})

add_library(imgui_app STATIC ${IMGUI_SOURCES}
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui_app PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui_app PUBLIC glfw OpenGL::GL)

add_executable(viewer viewer.cpp viewer_view.cpp)
target_link_libraries(viewer PRIVATE lockstep_net imgui_app)
target_compile_options(viewer PRIVATE ${LOCKSTEP_WARNINGS})

if(LOCKSTEP_TESTS)
  enable_testing()

  FetchContent_Declare(googletest GIT_REPOSITORY https://github.com/google/googletest.git GIT_TAG v1.17.0)
  FetchContent_Declare(imgui_test_engine GIT_REPOSITORY https://github.com/ocornut/imgui_test_engine.git
    GIT_TAG v1.93.0)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest imgui_test_engine)

  set(TEST_DEFINES
    LOCKSTEP_ENGINE="$<TARGET_FILE:engine>"
    LOCKSTEP_LLDB_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/tests/lldb_self_stop.py")

  add_executable(lockstep_tests tests/engine_test.cpp tests/net_test.cpp tests/debugger_test.cpp)
  target_link_libraries(lockstep_tests PRIVATE lockstep_engine GTest::gtest_main)
  target_include_directories(lockstep_tests PRIVATE tests)
  target_compile_definitions(lockstep_tests PRIVATE ${TEST_DEFINES})
  target_compile_options(lockstep_tests PRIVATE ${LOCKSTEP_WARNINGS})
  add_dependencies(lockstep_tests engine)
  include(GoogleTest)
  gtest_discover_tests(lockstep_tests DISCOVERY_MODE PRE_TEST)

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
    ${TE_DIR} ${TE_DIR}/imgui_test_engine
    ${TE_DIR}/shared ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_compile_definitions(imgui_te PUBLIC IMGUI_USER_CONFIG="imconfig_test_engine.h")
  target_link_libraries(imgui_te PUBLIC Threads::Threads)

  add_executable(viewer_ui_tests tests/viewer_ui_test.cpp viewer_view.cpp)
  target_link_libraries(viewer_ui_tests PRIVATE lockstep_engine imgui_te)
  target_compile_definitions(viewer_ui_tests PRIVATE ${TEST_DEFINES})
  target_compile_options(viewer_ui_tests PRIVATE ${LOCKSTEP_WARNINGS})
  add_dependencies(viewer_ui_tests engine)
  add_test(NAME viewer_ui_tests COMMAND viewer_ui_tests)
endif()
