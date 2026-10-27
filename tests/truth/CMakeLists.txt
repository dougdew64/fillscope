# Reading a replay's truth (truth_test.cpp): its layout, looking up an order's and a trade's
# clients, and why a file can't be read. That the replay writes the truth of a real session is
# checked by session_system_tests.
fillscope_gtest(truth_tests unit SOURCES truth_test.cpp LIBS fillscope_truth)
