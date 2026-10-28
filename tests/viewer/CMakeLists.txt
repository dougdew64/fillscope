# The viewer's core (selection, context, publishing, options), unit-tested without Dear ImGui.
fillscope_gtest(viewer_tests unit SOURCES viewer_test.cpp kept_files_test.cpp session_truth_test.cpp LIBS fillscope_viewer_core)
target_include_directories(viewer_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)

# The viewer's log and its crash recording, each crash in a process of its own (death tests).
fillscope_gtest(viewer_log_tests unit SOURCES viewer_log_test.cpp LIBS fillscope_viewer_core)

# The viewer's UI, under the Dear ImGui Test Engine (tests/CMakeLists.txt builds imgui_te). Not a
# GoogleTest program: it runs the Test Engine's own tests and exits nonzero if any fails.
add_executable(viewer_ui_tests viewer_ui_test.cpp ${VIEWER_UI_SOURCES})
target_link_libraries(viewer_ui_tests PRIVATE fillscope_viewer_core imgui_te)
target_include_directories(viewer_ui_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_compile_options(viewer_ui_tests PRIVATE ${FILLSCOPE_WARNINGS})
# In two parts, each a CTest test of its own, so neither takes more than half the time
# (viewer_ui_test.cpp says why): the live viewer's, and replays', answers' and sessions'.
fillscope_test_program(viewer_ui_tests ui)
fillscope_test_program(viewer_replay_ui_tests ui COMMAND $<TARGET_FILE:viewer_ui_tests> --replays
  RUNTIME_DEPS $<TARGET_FILE:viewer_ui_tests>)

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

# The live demo (viewer_demo --live): the viewer on the real exchange and a client trading at
# random, checked against the demo's own books; `--watch` shows it (the "demo: live trading"
# launch). The system tier, holding the exchange's lock: it runs an exchange. Not a program of its
# own, but viewer_demo run another way, so viewer_demo is among the files it uses.
fillscope_test_program(viewer_live_demo system
  COMMAND $<TARGET_FILE:viewer_demo> --live $<TARGET_FILE:exchange_main> $<TARGET_FILE:trading_main>
  RUNTIME_DEPS $<TARGET_FILE:viewer_demo> $<TARGET_FILE:exchange_main> $<TARGET_FILE:trading_main>)
set_tests_properties(viewer_live_demo PROPERTIES RESOURCE_LOCK exchange)

# The time-shifting demo (viewer_demo --time-shift): the live viewer paused, an order pointed at in the
# still book, stepped back to before it arrived and forward to see it arrive, played behind live, and
# back to live, checked at each against the demo's own books; `--watch` shows it (the "demo:
# time-shifting" launch). As the live demo: the system tier, the exchange's lock, viewer_demo among
# the files it uses.
fillscope_test_program(viewer_time_shift_demo system
  COMMAND $<TARGET_FILE:viewer_demo> --time-shift $<TARGET_FILE:exchange_main> $<TARGET_FILE:trading_main>
  RUNTIME_DEPS $<TARGET_FILE:viewer_demo> $<TARGET_FILE:exchange_main> $<TARGET_FILE:trading_main>)
set_tests_properties(viewer_time_shift_demo PROPERTIES RESOURCE_LOCK exchange)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# The slicing demo (viewer_demo --slicing): a session recorded (tools/sessions/record_session.py), then
# shown with its truth, the slicer's trades played and followed on each of its tickers, checked against
# the demo's own books and the truth; `--watch` shows it (the "demo: slicing" launch). The system tier,
# holding the exchange's lock, and alone, as the session test is: its session's programs spin
# (docs/TROUBLESHOOTING.md, case 2).
fillscope_test_program(viewer_slicing_demo system
  COMMAND $<TARGET_FILE:viewer_demo> --slicing ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/sessions/record_session.py
          $<TARGET_FILE:exchange_main> $<TARGET_FILE:trading_main> $<TARGET_FILE:exchange_replay>
  RUNTIME_DEPS $<TARGET_FILE:viewer_demo> ${PROJECT_SOURCE_DIR}/tools/sessions/record_session.py $<TARGET_FILE:exchange_main>
               $<TARGET_FILE:trading_main> $<TARGET_FILE:exchange_replay> ${Python3_EXECUTABLE})
set_tests_properties(viewer_slicing_demo PROPERTIES RESOURCE_LOCK exchange RUN_SERIAL TRUE)

# The viewer program itself (viewer_program_test.py): its window, paint loop, report lines, and every
# way it ends, on the real exchange and a client. The one test of src/viewer/main.cpp, which the UI
# tests and the demos don't run. The system tier, holding the exchange's lock. It opens the window on
# an Xvfb of its own, never the desktop's; without Xvfb it exits 77, reported as skipped.
fillscope_test_program(viewer_program_tests system
  COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/viewer_program_test.py
          $<TARGET_FILE:viewer> $<TARGET_FILE:exchange_main> $<TARGET_FILE:trading_main> $<TARGET_FILE:fillscope-books>
  RUNTIME_DEPS ${CMAKE_CURRENT_SOURCE_DIR}/viewer_program_test.py
               $<TARGET_FILE:viewer> $<TARGET_FILE:exchange_main>
               $<TARGET_FILE:trading_main> $<TARGET_FILE:fillscope-books> ${Python3_EXECUTABLE})
set_tests_properties(viewer_program_tests PROPERTIES RESOURCE_LOCK exchange SKIP_RETURN_CODE 77)
