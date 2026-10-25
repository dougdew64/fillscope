# The test machinery's own logic (iterate.py, and the mutation check's choice of mutants), tested
# with Python's unittest.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
fillscope_test_program(scripts_tests unit
  COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/scripts_test.py
  RUNTIME_DEPS ${CMAKE_CURRENT_SOURCE_DIR}/scripts_test.py ${CMAKE_CURRENT_SOURCE_DIR}/iterate.py
               ${CMAKE_CURRENT_SOURCE_DIR}/mutants.py ${CMAKE_CURRENT_SOURCE_DIR}/scratch_tree.py ${Python3_EXECUTABLE})
