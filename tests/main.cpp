#include <gtest/gtest.h>

/**
 * @brief Entry point for running GoogleTest unit test suite.
 * @param argc Command line argument count.
 * @param argv Command line argument vector.
 * @return 0 if all tests pass, non-zero otherwise.
 */
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
