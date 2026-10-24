# The back end (backend/, docs/PLAN.md) as Fillscope builds it.

# Its own tests (backend/Chapter12/tests/): the market data's layout, the matching, sequence numbers
# and snapshots. The whole program is one test, as the fork registers it (its objects take seconds
# and gigabytes to make, so a process per test would cost each), in the system tier: about 12 s,
# but about 40 s in the sanitizer build alongside the other tests.
fillscope_test_program(lowlatency_tests system COMMAND $<TARGET_FILE:lowlatency_tests>)

# What must not allocate (the queues, the pools, the price levels, the snapshot's updates), in a
# program of its own, since it replaces operator new. The system tier, as one test: making a
# snapshot synthesizer, which preallocates its pools, takes over a second.
fillscope_test_program(lowlatency_allocation_tests system COMMAND $<TARGET_FILE:lowlatency_allocation_tests>)

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

# Debugger lockstep with the matcher, on a replay (replay_debugger_test.cpp): exchange_replay under GDB
# and LLDB. Also exchange_replay with the anchor and everything it uses built at -O2, to check the
# anchor survives optimization (docs/DEBUGGER-LOCKSTEP.md, rule 1); without the sanitizers, whose own
# code changes what an optimized build can show a debugger.
set(BACKEND ${PROJECT_SOURCE_DIR}/backend/Chapter12)
add_executable(exchange_replay_o2 ${BACKEND}/exchange/exchange_replay.cpp
  ${BACKEND}/exchange/matcher/lockstep_anchor.cpp ${BACKEND}/exchange/matcher/matching_engine.cpp
  ${BACKEND}/exchange/matcher/me_order_book.cpp ${BACKEND}/exchange/matcher/me_order.cpp
  ${BACKEND}/exchange/matcher/replay.cpp ${BACKEND}/exchange/matcher/request_recording.cpp
  ${BACKEND}/common/mcast_socket.cpp)
target_include_directories(exchange_replay_o2 PRIVATE ${BACKEND} ${BACKEND}/exchange)
set_target_properties(exchange_replay_o2 PROPERTIES CXX_STANDARD 20)
target_compile_options(exchange_replay_o2 PRIVATE -O2 -g -fno-sanitize=all)
target_link_options(exchange_replay_o2 PRIVATE -fno-sanitize=all)
target_link_libraries(exchange_replay_o2 PRIVATE Threads::Threads)

set(request_anchor_source ${BACKEND}/exchange/matcher/lockstep_anchor.cpp)
fillscope_gtest(replay_debugger_tests debugger SOURCES replay_debugger_test.cpp
  RUNTIME_DEPS ${CMAKE_CURRENT_SOURCE_DIR}/replay_lldb_driver.py $<TARGET_FILE:exchange_replay>
               $<TARGET_FILE:exchange_replay_o2> ${request_anchor_source})
add_dependencies(replay_debugger_tests exchange_replay exchange_replay_o2)
target_compile_definitions(replay_debugger_tests PRIVATE
  FILLSCOPE_EXCHANGE_REPLAY="$<TARGET_FILE:exchange_replay>"
  FILLSCOPE_EXCHANGE_REPLAY_O2="$<TARGET_FILE:exchange_replay_o2>"
  FILLSCOPE_REPLAY_LLDB_DRIVER="${CMAKE_CURRENT_SOURCE_DIR}/replay_lldb_driver.py"
  FILLSCOPE_REQUEST_ANCHOR_SOURCE="${request_anchor_source}")
