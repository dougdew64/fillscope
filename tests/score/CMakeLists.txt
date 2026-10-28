# The detector's check (score_test.cpp): which clients and trades are targets, and sets of flags made
# by hand scored against a session made by hand. A real session's targets are checked by
# session_system_tests.
fillscope_gtest(score_tests unit SOURCES score_test.cpp LIBS fillscope_score fillscope_viewer_core)
