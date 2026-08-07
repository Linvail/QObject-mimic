#include <gtest/gtest.h>

//! Entry point for running GoogleTest unit test suite. Returns 0 if all tests pass, non-zero
//! otherwise.
int main
    (
    int argc,     //!< Command line argument count.
    char** argv   //!< Command line argument vector.
    )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
