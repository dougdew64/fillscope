# The viewer's core (selection, context, publishing, options), unit-tested without Dear ImGui.
fillscope_gtest(viewer_tests unit SOURCES viewer_test.cpp LIBS fillscope_viewer_core)

# The viewer's UI, under the Dear ImGui Test Engine (tests/CMakeLists.txt builds imgui_te). Not a
# GoogleTest program: it runs the Test Engine's own tests and exits nonzero if any fails.
add_executable(viewer_ui_tests viewer_ui_test.cpp ${VIEWER_UI_SOURCES})
target_link_libraries(viewer_ui_tests PRIVATE fillscope_viewer_core imgui_te)
target_include_directories(viewer_ui_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_compile_options(viewer_ui_tests PRIVATE ${FILLSCOPE_WARNINGS})
fillscope_test_program(viewer_ui_tests ui)

# The demo (viewer_demo.cpp): lesson 6 of the course played in the viewer with real clicks, checked
# at every frame. Headless here; `viewer_demo FILLSCOPE_ANSWER COURSE_DIR --watch` shows it in a
# window (the "demo: a market maker" launch). The system tier, holding the exchange's lock: it plays
# the lesson through the answer tool, which starts a replay.
add_executable(viewer_demo viewer_demo.cpp ${VIEWER_UI_SOURCES})
target_link_libraries(viewer_demo PRIVATE fillscope_viewer_core imgui_te)
target_include_directories(viewer_demo PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_compile_options(viewer_demo PRIVATE ${FILLSCOPE_WARNINGS})
fillscope_test_program(viewer_demo system
  COMMAND $<TARGET_FILE:viewer_demo> $<TARGET_FILE:fillscope-answer> ${PROJECT_SOURCE_DIR}/course
  RUNTIME_DEPS $<TARGET_FILE:fillscope-answer> $<TARGET_FILE:exchange_replay> ${PROJECT_SOURCE_DIR}/course/course.json
    ${PROJECT_SOURCE_DIR}/course/06-market-maker.json)
set_tests_properties(viewer_demo PROPERTIES RESOURCE_LOCK exchange)
