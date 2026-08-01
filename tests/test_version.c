/*
 * test_version.c — runtime version accessors match the compile-time macros.
 */
#include "utest.h"
#include <keel/keel.h>
#include <string.h>

UTEST(version, runtime_matches_macros) {
    ASSERT_STREQ(kl_version(), KL_VERSION_STRING);
    ASSERT_EQ(kl_version_number(), KL_VERSION_NUMBER);
}

UTEST(version, packed_number_is_monotone) {
    /* Packed form orders correctly for #if gating. */
    ASSERT_EQ(KL_VERSION_NUMBER,
              KL_VERSION_MAJOR * 10000 + KL_VERSION_MINOR * 100 + KL_VERSION_PATCH);
    ASSERT_TRUE(KL_VERSION_NUMBER > 0);
}

UTEST_MAIN();
