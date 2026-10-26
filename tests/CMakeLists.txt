# The tests: one directory per area, mirroring src/ and tools/, plus support/ (shared helpers) and
# scripts/ (the commands that run and check them; docs/RUNNING.md).

# Release archives checked against their SHA-256, as in the top-level CMakeLists.txt (which fetches
# GoogleTest, before the back end).
FetchContent_Declare(imgui_test_engine
  URL https://github.com/ocornut/imgui_test_engine/archive/refs/tags/v1.93.0.tar.gz
  URL_HASH SHA256=1a65938e1b9a6f5f154ec1f92a6b5a14ea6ef6b4247ca22cd166c19e5befd6a5)
FetchContent_MakeAvailable(imgui_test_engine)

include(GoogleTest)

# Every test program is registered through fillscope_test_program, which gives its tests:
#   - a cost tier, as a label and a per-test timeout, so a slow test fails as slow the day it
#     is added: unit (2 s), ui (10 s, the whole program), check (30 s, a whole checker run such
#     as a type check), debugger (30 s), system (120 s, the back end's programs);
#   - a label naming the program, exe:<name>, so tests/scripts/iterate.py can run one program's
#     tests;
#   - a line in test-manifest.txt: the program, its tier, whether it always runs, and the files
#     it uses at run time.
# iterate.py re-runs a program only when the program, one of those files, a shared library it
# loads, a FILLSCOPE_ environment variable, or any CMakeLists.txt (which can change how any test
# runs) has changed, or on every iteration if it is ALWAYS (its result depends on files it can't
# list).
# tests/scripts/iterate.py --check-registration (run by run-tests.sh) fails on any test registered
# some other way.
# A program's tests are all in one tier, so a program is either fast or slow, never both.
# Programs are named for the area they test (feed_tests); when an area's tests span tiers, the
# slower program adds the tier (feed_system_tests).
set(FILLSCOPE_TIMEOUT_unit 2)
set(FILLSCOPE_TIMEOUT_ui 10)
set(FILLSCOPE_TIMEOUT_debugger 30)
set(FILLSCOPE_TIMEOUT_check 30)
set(FILLSCOPE_TIMEOUT_system 120)

# Registers test program `name`: a GoogleTest program (GTEST), a program built here (neither
# option), or a command that isn't built here (COMMAND, e.g. Node running the extension's tests;
# its RUNTIME_DEPS are then everything it reads, since there is no binary to fingerprint).
function(fillscope_test_program name tier)
  cmake_parse_arguments(ARG "GTEST;ALWAYS" "" "RUNTIME_DEPS;COMMAND" ${ARGN})
  if(NOT DEFINED FILLSCOPE_TIMEOUT_${tier})
    message(FATAL_ERROR "${name}: unknown test tier '${tier}'")
  endif()
  set(labels "${tier}\\;exe:${name}")
  if(ARG_GTEST)
    gtest_discover_tests(${name} DISCOVERY_MODE PRE_TEST
      PROPERTIES LABELS "${labels}" TIMEOUT ${FILLSCOPE_TIMEOUT_${tier}})
  else()
    if(ARG_COMMAND)
      add_test(NAME ${name} COMMAND ${ARG_COMMAND})
    else()
      add_test(NAME ${name} COMMAND ${name})
    endif()
    set_tests_properties(${name} PROPERTIES LABELS "${tier};exe:${name}" TIMEOUT ${FILLSCOPE_TIMEOUT_${tier}})
  endif()
  if(TARGET ${name})
    set(program $<TARGET_FILE:${name}>)
  else()
    set(program "-")  # a command, not a program built here
  endif()
  if(ARG_ALWAYS)
    set(flags always)
  else()
    set(flags -)
  endif()
  # Tab-separated, so a path may contain spaces.
  list(JOIN ARG_RUNTIME_DEPS "\t" runtime_deps)
  set_property(GLOBAL APPEND PROPERTY FILLSCOPE_TEST_MANIFEST "${name}\t${tier}\t${flags}\t${program}\t${runtime_deps}")
endfunction()

# A GoogleTest program: its sources, the libraries it tests, and the files it uses at run time.
# ALWAYS is passed on to fillscope_test_program.
function(fillscope_gtest name tier)
  cmake_parse_arguments(ARG "ALWAYS" "" "SOURCES;LIBS;RUNTIME_DEPS" ${ARGN})
  add_executable(${name} ${ARG_SOURCES})
  target_link_libraries(${name} PRIVATE ${ARG_LIBS} GTest::gtest_main)
  target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/tests)
  target_compile_options(${name} PRIVATE ${FILLSCOPE_WARNINGS})
  set(options)  # the option, passed on if given
  if(ARG_ALWAYS)
    list(APPEND options ALWAYS)
  endif()
  fillscope_test_program(${name} ${tier} GTEST ${options} RUNTIME_DEPS ${ARG_RUNTIME_DEPS})
endfunction()

# Dear ImGui again, built with the Test Engine's hooks (viewer/imconfig_test_engine.h), plus the
# Test Engine and its app layer: the null backend, real Dear ImGui frames with no window, for the
# tests; and GLFW with OpenGL 3, a real window, for the demo when watched. Kept apart from imgui_app
# so the viewer itself has no hooks.
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
  ${TE_DIR} ${TE_DIR}/imgui_test_engine ${TE_DIR}/shared ${CMAKE_CURRENT_SOURCE_DIR}/viewer)
target_compile_definitions(imgui_te PUBLIC IMGUI_USER_CONFIG="imconfig_test_engine.h" IMGUI_APP_GLFW_GL3)
target_link_libraries(imgui_te PUBLIC Threads::Threads glfw OpenGL::GL)

add_subdirectory(answer)
add_subdirectory(attribution)
add_subdirectory(code_map)
add_subdirectory(common)
add_subdirectory(backend)
add_subdirectory(docs)
add_subdirectory(feed)
add_subdirectory(viewer)
add_subdirectory(vscode-debug-state)
add_subdirectory(scripts)

# One line per test program, tab-separated: name, tier, "always" or "-", program ("-" for a
# command), run-time files. Read by scripts/iterate.py and scripts/run-tests.sh.
get_property(manifest GLOBAL PROPERTY FILLSCOPE_TEST_MANIFEST)
list(JOIN manifest "\n" manifest)
file(GENERATE OUTPUT ${PROJECT_BINARY_DIR}/test-manifest.txt CONTENT "${manifest}\n")
