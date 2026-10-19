# The viewer's UI, under the Dear ImGui Test Engine (tests/CMakeLists.txt builds imgui_te). Not a
# GoogleTest program: it runs the Test Engine's own tests and exits nonzero if any fails.
add_executable(viewer_ui_tests viewer_ui_test.cpp ${PROJECT_SOURCE_DIR}/src/viewer/viewer_view.cpp)
target_link_libraries(viewer_ui_tests PRIVATE frame_source_lib imgui_te)
target_include_directories(viewer_ui_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_compile_definitions(viewer_ui_tests PRIVATE ${TEST_DEFINES})
target_compile_options(viewer_ui_tests PRIVATE ${FILLSCOPE_WARNINGS})
add_dependencies(viewer_ui_tests frame_source)
fillscope_test_program(viewer_ui_tests ui RUNTIME_DEPS $<TARGET_FILE:frame_source>)
