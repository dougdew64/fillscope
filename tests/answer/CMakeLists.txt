# The answer tool's parts (answer_test.cpp): reading a scenario, the back end's files as bytes, and
# checking claims.
fillscope_gtest(answer_tests unit SOURCES answer_test.cpp LIBS fillscope_answer)

# The tool run whole, through the back end's real exchange_replay (answer_system_test.cpp). The
# system tier: each run starts a replay, which builds a matching engine (about 1.3 s and 800 MB, 4 s
# in the sanitizer build), so it holds the lock every test that runs the exchange holds.
add_executable(answer_system_tests answer_system_test.cpp)
target_link_libraries(answer_system_tests PRIVATE nlohmann_json::nlohmann_json GTest::gtest)
target_include_directories(answer_system_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_compile_options(answer_system_tests PRIVATE ${FILLSCOPE_WARNINGS})
fillscope_test_program(answer_system_tests system
  COMMAND $<TARGET_FILE:answer_system_tests> $<TARGET_FILE:fillscope-answer>
  RUNTIME_DEPS $<TARGET_FILE:fillscope-answer> $<TARGET_FILE:exchange_replay>)
set_tests_properties(answer_system_tests PROPERTIES RESOURCE_LOCK exchange)

# The course (course_system_test.cpp): every lesson played through the tool and exchange_replay, and
# read back as the viewer reads it, every claim checked again; and the manifest naming exactly the
# lessons there are. The system tier, holding the exchange's lock: each lesson starts a replay.
file(GLOB course_files CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/course/*.json)
add_executable(course_system_tests course_system_test.cpp)
target_link_libraries(course_system_tests PRIVATE fillscope_viewer_core GTest::gtest)
target_include_directories(course_system_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_compile_options(course_system_tests PRIVATE ${FILLSCOPE_WARNINGS})
fillscope_test_program(course_system_tests system
  COMMAND $<TARGET_FILE:course_system_tests> $<TARGET_FILE:fillscope-answer> ${PROJECT_SOURCE_DIR}/course
  RUNTIME_DEPS $<TARGET_FILE:fillscope-answer> $<TARGET_FILE:exchange_replay> ${course_files})
set_tests_properties(course_system_tests PROPERTIES RESOURCE_LOCK exchange)
