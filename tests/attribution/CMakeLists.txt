# The attribution report (tools/attribution/): the script's own tests, and a check that the
# committed docs/ATTRIBUTION.md is exactly what its named commit gives. The check's result depends
# on the repository's history, which no run-time file stands for, so it is ALWAYS run.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(ATTRIBUTION_SCRIPT ${PROJECT_SOURCE_DIR}/tools/attribution/attribution.py)
fillscope_test_program(attribution_tests check
  COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/attribution_test.py
  RUNTIME_DEPS ${CMAKE_CURRENT_SOURCE_DIR}/attribution_test.py ${ATTRIBUTION_SCRIPT} ${Python3_EXECUTABLE})
# The back end keeps a copy of the script, to run in its own CI; it must not drift from this one.
# A test of its own, so that a change to the script (a mutant, say) isn't caught here instead of by
# attribution_tests.
set(ATTRIBUTION_COPY ${PROJECT_SOURCE_DIR}/backend/tools/attribution/attribution.py)
fillscope_test_program(attribution_copy_check unit
  COMMAND ${CMAKE_COMMAND} -E compare_files ${ATTRIBUTION_SCRIPT} ${ATTRIBUTION_COPY}
  RUNTIME_DEPS ${ATTRIBUTION_SCRIPT} ${ATTRIBUTION_COPY})
fillscope_test_program(attribution_report_check check ALWAYS
  COMMAND ${Python3_EXECUTABLE} -I ${ATTRIBUTION_SCRIPT} --check
  RUNTIME_DEPS ${ATTRIBUTION_SCRIPT} ${PROJECT_SOURCE_DIR}/docs/ATTRIBUTION.md ${Python3_EXECUTABLE})
