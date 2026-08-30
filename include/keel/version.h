/*
 * keel/version.h - Keel version macros. GENERATED from the root VERSION file by
 * tools/version_sync.sh. DO NOT EDIT: change VERSION and run `make version-sync`.
 * The check-version-drift gate fails if this file disagrees with VERSION.
 *
 * Freestanding-safe: macros only, no includes. Both umbrellas (keel.h, freestanding.h)
 * include this. KL_VERSION_NUMBER is numeric (major*10000 + minor*100 + patch); a
 * prerelease appears only in KL_VERSION_STRING and KL_VERSION_PRERELEASE.
 */
#ifndef KEEL_VERSION_H
#define KEEL_VERSION_H

#define KL_VERSION_MAJOR  3
#define KL_VERSION_MINOR  0
#define KL_VERSION_PATCH  0
#define KL_VERSION_STRING "3.0.0-rc.1"
#define KL_VERSION_PRERELEASE "rc.1"
#define KL_VERSION_IS_PRERELEASE 1
#define KL_VERSION_NUMBER \
    ((KL_VERSION_MAJOR) * 10000 + (KL_VERSION_MINOR) * 100 + (KL_VERSION_PATCH))

#endif /* KEEL_VERSION_H */
