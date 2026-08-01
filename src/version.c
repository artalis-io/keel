/*
 * version.c — runtime version accessors.
 *
 * The KL_VERSION_* macros in keel.h describe the headers a consumer compiled
 * against; these functions report the version compiled into the *library*, so a
 * consumer that static-relinks against a newer Keel can verify it at runtime.
 */
#include <keel/keel.h>

const char *kl_version(void) {
    return KL_VERSION_STRING;
}

int kl_version_number(void) {
    return KL_VERSION_NUMBER;
}
