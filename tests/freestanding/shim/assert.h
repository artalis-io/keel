#ifndef KEEL_FS_SHIM_ASSERT_H
#define KEEL_FS_SHIM_ASSERT_H
/*
 * Freestanding <assert.h> shim (declarations only — see README.md).
 *
 * -ffreestanding provides no <assert.h>. Reduce assert() to a no-op for the
 * cross-target symbol-gate build; a real UEFI/EDK2 build maps it to ASSERT() /
 * CpuDeadLoop(). No hosted runtime, no __assert_fail dependency.
 */
#define assert(expr) ((void)0)
#endif /* KEEL_FS_SHIM_ASSERT_H */
