# The attribution report (tools/attribution/): the script's own tests, and a check that the
# committed docs/ATTRIBUTION.md is exactly what its named commit gives. The check's result depends
# on the repository's history, which no run-time file stands for, so it is ALWAYS run.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(ATTRIBUTION_SCRIPT ${PROJECT_SOURCE_DIR}/tools/attribution/attribution.py)
fillscope_test_program(attribution_tests check
  COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/attribution_test.py
  RUNTIME_DEPS ${CMAKE_CURRENT_SOURCE_DIR}/attribution_test.py ${ATTRIBUTION_SCRIPT} ${Python3_EXECUTABLE})
fillscope_test_program(attribution_report_check check ALWAYS
  COMMAND ${Python3_EXECUTABLE} -I ${ATTRIBUTION_SCRIPT} --check
  RUNTIME_DEPS ${ATTRIBUTION_SCRIPT} ${PROJECT_SOURCE_DIR}/docs/ATTRIBUTION.md ${Python3_EXECUTABLE})
