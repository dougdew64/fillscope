fillscope_gtest(feed_tests unit SOURCES feed_test.cpp LIBS fillscope_feed)

# What the viewer keeps of the market data, to show the books after any update (time_shift.h).
fillscope_gtest(time_shift_tests unit SOURCES time_shift_test.cpp LIBS fillscope_feed)

# Fillscope's books against the real exchange's, and its recording replayed (feed_system_test.cpp).
# The system tier: about 6 s, most of it the three snapshots, a second apart, that it waits for once
# trading has begun. It runs an exchange and a client, on ports of its own; the exchange alone takes about 2 GB of memory, so it holds the lock every test that runs one
# holds, and two never run at once.
add_executable(feed_system_tests feed_system_test.cpp)
target_link_libraries(feed_system_tests PRIVATE fillscope_feed GTest::gtest)
target_include_directories(feed_system_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_compile_options(feed_system_tests PRIVATE ${FILLSCOPE_WARNINGS})
fillscope_test_program(feed_system_tests system
  COMMAND $<TARGET_FILE:feed_system_tests> $<TARGET_FILE:exchange_main> $<TARGET_FILE:trading_main>
          $<TARGET_FILE:exchange_replay>
  RUNTIME_DEPS $<TARGET_FILE:exchange_main> $<TARGET_FILE:trading_main> $<TARGET_FILE:exchange_replay>)
set_tests_properties(feed_system_tests PROPERTIES RESOURCE_LOCK exchange)
