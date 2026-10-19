fillscope_gtest(lockstep_tests unit SOURCES lockstep_test.cpp LIBS fillscope_lockstep)

# The anchor under real debuggers, driven through the frame source: slow, so a program of its own.
# The frame source again, with the anchor and everything it uses built at -O2, for the debugger
# tests that check the anchor survives optimization (docs/DEBUGGER-LOCKSTEP.md, rule 1). Always
# without the sanitizers, even in the sanitizer build: the question is what optimization does, and
# the sanitizers' own code changes what an optimized build can show a debugger (under them, GCC
# 16.2.1 no longer describes where frame_index is at the self-stop).
add_executable(frame_source_o2 ${PROJECT_SOURCE_DIR}/tools/frame_source/main.cpp
  ${PROJECT_SOURCE_DIR}/tools/frame_source/frame_source.cpp ${PROJECT_SOURCE_DIR}/src/lockstep/anchor.cpp
  ${PROJECT_SOURCE_DIR}/src/channel/message.cpp ${PROJECT_SOURCE_DIR}/src/channel/net.cpp)
target_include_directories(frame_source_o2 PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/tools/frame_source)
target_compile_options(frame_source_o2 PRIVATE -O2 -g -fno-sanitize=all ${FILLSCOPE_WARNINGS})
target_link_options(frame_source_o2 PRIVATE -fno-sanitize=all)

set(anchor_source ${PROJECT_SOURCE_DIR}/src/lockstep/anchor.cpp)
fillscope_gtest(lockstep_debugger_tests debugger USES_FRAME_SOURCE SOURCES lockstep_debugger_test.cpp
  LIBS fillscope_channel
  RUNTIME_DEPS ${CMAKE_CURRENT_SOURCE_DIR}/lldb_driver.py $<TARGET_FILE:frame_source_o2> ${anchor_source})
add_dependencies(lockstep_debugger_tests frame_source_o2)
target_compile_definitions(lockstep_debugger_tests PRIVATE
  FILLSCOPE_LLDB_DRIVER="${CMAKE_CURRENT_SOURCE_DIR}/lldb_driver.py"
  FILLSCOPE_FRAME_SOURCE_O2="$<TARGET_FILE:frame_source_o2>"
  FILLSCOPE_ANCHOR_SOURCE="${anchor_source}")
