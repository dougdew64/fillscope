# The code map (tools/code_map/): the check's own tests, and the check of docs/CODE-MAP.md. The
# check's result depends on which files the repository has, which no run-time file stands for, so
# it is ALWAYS run: it takes a fraction of a second.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(CODE_MAP_SCRIPT ${PROJECT_SOURCE_DIR}/tools/code_map/code_map.py)
fillscope_test_program(code_map_tests unit
  COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/code_map_test.py
  RUNTIME_DEPS ${CMAKE_CURRENT_SOURCE_DIR}/code_map_test.py ${CODE_MAP_SCRIPT} ${Python3_EXECUTABLE})
fillscope_test_program(code_map_check unit ALWAYS
  COMMAND ${Python3_EXECUTABLE} -I ${CODE_MAP_SCRIPT} --check ${PROJECT_SOURCE_DIR}
  RUNTIME_DEPS ${CODE_MAP_SCRIPT} ${PROJECT_SOURCE_DIR}/docs/CODE-MAP.md ${Python3_EXECUTABLE})
