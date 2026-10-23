fillscope_gtest(feed_tests unit SOURCES feed_test.cpp LIBS fillscope_feed)

# Fillscope's books against the real exchange's (feed_system_test.cpp). The system tier: about half a
# minute, mostly the back end's own start-up waits. It runs an exchange, whose ports are fixed in its
# code, so it holds the lock every test that runs one holds.
add_executable(feed_system_tests feed_system_test.cpp)
target_link_libraries(feed_system_tests PRIVATE fillscope_feed GTest::gtest)
target_compile_options(feed_system_tests PRIVATE ${FILLSCOPE_WARNINGS})
fillscope_test_program(feed_system_tests system
  COMMAND $<TARGET_FILE:feed_system_tests> $<TARGET_FILE:exchange_main> $<TARGET_FILE:trading_main>
  RUNTIME_DEPS $<TARGET_FILE:exchange_main> $<TARGET_FILE:trading_main>)
set_tests_properties(feed_system_tests PROPERTIES RESOURCE_LOCK exchange)
