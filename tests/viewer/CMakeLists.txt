# The viewer's core (selection, context, publishing, options), unit-tested without Dear ImGui.
fillscope_gtest(viewer_tests unit SOURCES viewer_test.cpp LIBS fillscope_viewer_core)

# The viewer's UI, under the Dear ImGui Test Engine (tests/CMakeLists.txt builds imgui_te). Not a
# GoogleTest program: it runs the Test Engine's own tests and exits nonzero if any fails.
add_executable(viewer_ui_tests viewer_ui_test.cpp ${VIEWER_UI_SOURCES})
target_link_libraries(viewer_ui_tests PRIVATE fillscope_viewer_core imgui_te)
target_include_directories(viewer_ui_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_compile_options(viewer_ui_tests PRIVATE ${FILLSCOPE_WARNINGS})
fillscope_test_program(viewer_ui_tests ui)
