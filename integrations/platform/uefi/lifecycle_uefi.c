/*
 * lifecycle_uefi.c - kl_uefi_shutdown(): release the EFI network stack.
 * See lifecycle_uefi.h. Orchestrates the three provider/platform teardowns in
 * dependency order (sockets + events first, platform clock/EBS last).
 */
#include "lifecycle_uefi.h"
#include "socket_efi_tcp4.h"   /* kl_uefi_socket_provider_reset */
#include "event_efi.h"         /* kl_uefi_event_provider_reset */
#include "platform_uefi.h"     /* kl_uefi_platform_shutdown */

void kl_uefi_shutdown(void) {
    kl_uefi_socket_provider_reset();   /* free the TCP4 ServiceBinding handle buffer */
    kl_uefi_event_provider_reset();    /* clear the completion event provider state */
    kl_uefi_platform_shutdown();       /* cancel + close the timer + EBS event */
}
