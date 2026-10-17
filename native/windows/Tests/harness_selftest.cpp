// Proves the harness macros register and run; these should always pass.
#include "TestHarness.h"

TEST(harness_check_passes)
{
    CHECK(1 + 1 == 2);
}

TEST(harness_check_eq_passes)
{
    CHECK_EQ(3, 3);
}
