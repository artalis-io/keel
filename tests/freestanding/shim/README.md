# freestanding cross-target shim headers

Minimal, freestanding-safe declarations for the few hosted C-library headers
that clang's `-ffreestanding` mode does NOT provide (`string.h`, `stdlib.h`,
`stdio.h`, `sys/types.h`, `errno.h`) plus the Winsock type/constant shims the
internal `src/sockcompat.h` `_WIN32` branch expects (`winsock2.h`, `ws2tcpip.h`,
`afunix.h`).

Used ONLY by the cross-target freestanding build/link gates
(`make freestanding-lib` / `freestanding-lib-selfcontained` / `freestanding-link`
over the `x86_64-unknown-windows` / `aarch64-unknown-windows` triples), injected
via `-isystem`. A real UEFI/EDK2 build supplies the equivalents (BaseMemoryLib,
the EFI socket protocols). These carry NO logic — only the declarations the
freestanding client manifest references — so the archive's undefined-symbol
closure (memcpy/memmove/memset/memcmp/strlen + the KEEL seams) is unchanged.

They are deliberately NOT on the hosted build's include path.
