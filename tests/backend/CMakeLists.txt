# The back end (backend/, docs/PLAN.md) as Fillscope builds it: its exchange and a client, run
# together. The system tier: it takes half a minute, mostly the programs' own start-up waits.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
fillscope_test_program(backend_system_tests system
  COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/backend_system_test.py
          $<TARGET_FILE:exchange_main> $<TARGET_FILE:trading_main>
  RUNTIME_DEPS ${CMAKE_CURRENT_SOURCE_DIR}/backend_system_test.py $<TARGET_FILE:exchange_main>
               $<TARGET_FILE:trading_main> ${Python3_EXECUTABLE})
