/*
 * lifecycle_uefi.h — one-call teardown of the KEEL EFI network stack (U-7).
 *
 * The EFI provider is BOOT-SERVICES-ONLY: EFI_TCP4/UDP4, AllocatePool, and events all
 * vanish at ExitBootServices(). A UEFI app that transitions to OS runtime (an HTTP-boot
 * loader) must release every boot-services resource KEEL holds BEFORE calling
 * ExitBootServices(), and never touch the providers after. kl_uefi_shutdown() is that
 * teardown; the platform additionally registers an EVT_SIGNAL_EXIT_BOOT_SERVICES notify
 * (platform_uefi.c) so the stack degrades fail-closed even if the app forgets.
 */
#ifndef KEEL_UEFI_LIFECYCLE_UEFI_H
#define KEEL_UEFI_LIFECYCLE_UEFI_H

/*
 * Release the whole EFI network stack: the EFI_TCP4 socket provider (its
 * ServiceBinding handle buffer), the completion event provider, and the platform
 * (periodic monotonic timer + EBS event). Precondition: all connections already
 * closed (the async client closes its fd on completion). Call BEFORE
 * ExitBootServices(). Idempotent — safe to call more than once.
 */
void kl_uefi_shutdown(void);

#endif /* KEEL_UEFI_LIFECYCLE_UEFI_H */
