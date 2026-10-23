# The tests: one directory per area, mirroring src/ and tools/, plus support/ (shared helpers) and
# scripts/ (the commands that run and check them; docs/RUNNING.md).

# Release archives checked against their SHA-256, as in the top-level CMakeLists.txt.
FetchContent_Declare(googletest URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
  URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c)
FetchContent_Declare(imgui_test_engine
  URL https://github.com/ocornut/imgui_test_engine/archive/refs/tags/v1.93.0.tar.gz
  URL_HASH SHA256=1a65938e1b9a6f5f154ec1f92a6b5a14ea6ef6b4247ca22cd166c19e5befd6a5)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest imgui_test_engine)

include(GoogleTest)

# Every test program is registered through fillscope_test_program, which gives its tests:
#   - a cost tier, as a label and a per-test timeout, so a slow test fails as slow the day it
#     is added: unit (2 s), ui (10 s, the whole program), check (30 s, a whole checker run such
#     as a type check), debugger (30 s);
#   - a label naming the program, exe:<name>, so tests/scripts/iterate.py can run one program's
#     tests;
#   - a line in test-manifest.txt: the program, its tier, whether it always runs, and the files
#     it uses at run time.
# iterate.py re-runs a program only when the program, one of those files, a shared library it
# loads, a FILLSCOPE_ environment variable, or any CMakeLists.txt (which can change how any test
# runs) has changed, or on every iteration if it is ALWAYS (its result depends on files it can't
# list). A program that runs
# the frame source says so (USES_FRAME_SOURCE): it then gets the frame source's path, is built
# after it, and lists it as a run-time file, so all three can't fall out of step.
# tests/scripts/iterate.py --check-registration (run by run-tests.sh) fails on any test registered
# some other way.
# A program's tests are all in one tier, so a program is either fast or slow, never both.
# Programs are named for the area they test (channel_tests); when an area's tests span tiers, the
# slower program adds the tier (lockstep_debugger_tests).
set(FILLSCOPE_TIMEOUT_unit 2)
set(FILLSCOPE_TIMEOUT_ui 10)
set(FILLSCOPE_TIMEOUT_debugger 30)
set(FILLSCOPE_TIMEOUT_check 30)

# Registers test program `name`: a GoogleTest program (GTEST), a program built here (neither
# option), or a command that isn't built here (COMMAND, e.g. Node running the extension's tests;
# its RUNTIME_DEPS are then everything it reads, since there is no binary to fingerprint).
function(fillscope_test_program name tier)
  cmake_parse_arguments(ARG "GTEST;ALWAYS;USES_FRAME_SOURCE" "" "RUNTIME_DEPS;COMMAND" ${ARGN})
  if(NOT DEFINED FILLSCOPE_TIMEOUT_${tier})
    message(FATAL_ERROR "${name}: unknown test tier '${tier}'")
  endif()
  if(ARG_USES_FRAME_SOURCE)
    target_compile_definitions(${name} PRIVATE FILLSCOPE_FRAME_SOURCE="$<TARGET_FILE:frame_source>")
    add_dependencies(${name} frame_source)
    list(APPEND ARG_RUNTIME_DEPS $<TARGET_FILE:frame_source>)
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
# ALWAYS and USES_FRAME_SOURCE are passed on to fillscope_test_program.
function(fillscope_gtest name tier)
  cmake_parse_arguments(ARG "ALWAYS;USES_FRAME_SOURCE" "" "SOURCES;LIBS;RUNTIME_DEPS" ${ARGN})
  add_executable(${name} ${ARG_SOURCES})
  target_link_libraries(${name} PRIVATE ${ARG_LIBS} GTest::gtest_main)
  target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/tests)
  target_compile_options(${name} PRIVATE ${FILLSCOPE_WARNINGS})
  set(options)  # the options given, passed on as given
  foreach(option ALWAYS USES_FRAME_SOURCE)
    if(ARG_${option})
      list(APPEND options ${option})
    endif()
  endforeach()
  fillscope_test_program(${name} ${tier} GTEST ${options} RUNTIME_DEPS ${ARG_RUNTIME_DEPS})
endfunction()

# Dear ImGui again, built with the Test Engine's hooks (viewer/imconfig_test_engine.h), plus the
# Test Engine and its null backend: real Dear ImGui frames with no window. Kept apart from
# imgui_app so the viewer itself has no hooks.
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
target_compile_definitions(imgui_te PUBLIC IMGUI_USER_CONFIG="imconfig_test_engine.h")
target_link_libraries(imgui_te PUBLIC Threads::Threads)

add_subdirectory(attribution)
add_subdirectory(channel)
add_subdirectory(docs)
add_subdirectory(lockstep)
add_subdirectory(frame_source)
add_subdirectory(viewer)
add_subdirectory(vscode-debug-state)
add_subdirectory(scripts)

# One line per test program, tab-separated: name, tier, "always" or "-", program ("-" for a
# command), run-time files. Read by scripts/iterate.py and scripts/run-tests.sh.
get_property(manifest GLOBAL PROPERTY FILLSCOPE_TEST_MANIFEST)
list(JOIN manifest "\n" manifest)
file(GENERATE OUTPUT ${PROJECT_BINARY_DIR}/test-manifest.txt CONTENT "${manifest}\n")
