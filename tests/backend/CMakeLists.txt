# The back end (backend/, docs/PLAN.md) as Fillscope builds it.

# Its own tests (backend/Chapter12/tests/): the market data's layout, the matching, sequence numbers
# and snapshots. The whole program is one test, as the fork registers it (its objects take seconds
# and gigabytes to make, so a process per test would cost each), in the system tier: about 12 s,
# but about 40 s in the sanitizer build alongside the other tests.
fillscope_test_program(lowlatency_tests system COMMAND $<TARGET_FILE:lowlatency_tests>)

# Its exchange and a client, run together. The system tier: it takes half a minute, mostly the
# programs' own start-up waits.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
fillscope_test_program(backend_system_tests system
  COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/backend_system_test.py
          $<TARGET_FILE:exchange_main> $<TARGET_FILE:trading_main>
  RUNTIME_DEPS ${CMAKE_CURRENT_SOURCE_DIR}/backend_system_test.py $<TARGET_FILE:exchange_main>
               $<TARGET_FILE:trading_main> ${Python3_EXECUTABLE})
# It runs an exchange, whose ports are fixed in its code: one test at a time may (feed_system_tests too).
set_tests_properties(backend_system_tests PROPERTIES RESOURCE_LOCK exchange)
