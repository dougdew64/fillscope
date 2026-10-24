# The shared helpers in src/common: the descriptor owner and the number parser (common_test.cpp).
fillscope_gtest(common_tests unit SOURCES common_test.cpp)
target_include_directories(common_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
