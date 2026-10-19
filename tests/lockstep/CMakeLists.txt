fillscope_gtest(lockstep_tests unit SOURCES lockstep_test.cpp LIBS fillscope_lockstep)

# The anchor under real debuggers, driven through the frame source: slow, so a program of its own.
fillscope_gtest(lockstep_debugger_tests debugger SOURCES lockstep_debugger_test.cpp LIBS fillscope_channel
  RUNTIME_DEPS $<TARGET_FILE:frame_source> ${CMAKE_CURRENT_SOURCE_DIR}/lldb_self_stop.py)
target_compile_definitions(lockstep_debugger_tests PRIVATE
  FILLSCOPE_LLDB_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/lldb_self_stop.py")
add_dependencies(lockstep_debugger_tests frame_source)
