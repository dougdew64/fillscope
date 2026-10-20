# USES_FRAME_SOURCE: one test runs the program itself, to check its command line's handling.
fillscope_gtest(frame_source_tests unit USES_FRAME_SOURCE SOURCES frame_source_test.cpp LIBS frame_source_lib)
