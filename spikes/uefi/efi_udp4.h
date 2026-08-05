/*
 * efi_udp4.h — minimal EFI_UDP4 + ServiceBinding protocol surface for the U-5 spike.
 *
 * Vendored from the UEFI Specification 2.10, section 30.2 (EFI_UDP4_PROTOCOL) plus
 * the shared §28.1 EFI_SERVICE_BINDING_PROTOCOL / §28.3 EFI_IPv4_ADDRESS types. The
 * structural template is spikes/uefi/efi_tcp4.h (same ServiceBinding + child/token/
 * Poll lifecycle); this header adds only the UDP4-specific config + I/O tokens.
 *
 * Depends on efi_min.h (EFI_STATUS, EFI_EVENT, EFI_GUID, EFI_HANDLE, EFIAPI, BOOLEAN,
 * UINT8/16/32, UINTN, VOID) and efi_tcp4.h (for EFI_IPv4_ADDRESS +
 * EFI_SERVICE_BINDING_PROTOCOL, which the TCP4 header already defines — we reuse them
 * rather than re-vendoring, so the two stay in sync).
 *
 * EFIAPI is gated on __x86_64__ in efi_min.h (ms_abi only on x86-64).
 */
#ifndef SPIKE_EFI_UDP4_H
#define SPIKE_EFI_UDP4_H

/* Requires efi_tcp4.h first for EFI_IPv4_ADDRESS + EFI_SERVICE_BINDING_PROTOCOL. */
#include "efi_tcp4.h"

/* ---- GUIDs (UEFI 2.10 §30.1 / Appendix A) ---- */

/* EFI_UDP4_SERVICE_BINDING_PROTOCOL_GUID  83f01464-99bd-45e5-b383-af6305d8e9e6 */
#define EFI_UDP4_SERVICE_BINDING_PROTOCOL_GUID \
    { 0x83f01464, 0x99bd, 0x45e5, {0xb3, 0x83, 0xaf, 0x63, 0x05, 0xd8, 0xe9, 0xe6} }

/* EFI_UDP4_PROTOCOL_GUID                   3ad9df29-4501-478d-b1f8-7f7fe70e50f3 */
#define EFI_UDP4_PROTOCOL_GUID \
    { 0x3ad9df29, 0x4501, 0x478d, {0xb1, 0xf8, 0x7f, 0x7f, 0xe7, 0x0e, 0x50, 0xf3} }

/* ---- EFI_TIME (UEFI 2.10 §8.3 — RxData TimeStamp; layout only, unused here) ---- */
typedef struct {
    UINT16 Year;
    UINT8  Month;
    UINT8  Day;
    UINT8  Hour;
    UINT8  Minute;
    UINT8  Second;
    UINT8  Pad1;
    UINT32 Nanosecond;
    INT16  TimeZone;
    UINT8  Daylight;
    UINT8  Pad2;
} EFI_TIME;

/* ---- UDP4 config data (UEFI 2.10 §30.2.5, EFI_UDP4_CONFIG_DATA) ----
 * Field order is per-spec — a wrong layout silently corrupts Configure(). */
typedef struct {
    BOOLEAN          AcceptBroadcast;
    BOOLEAN          AcceptPromiscuous;
    BOOLEAN          AcceptAnyPort;
    BOOLEAN          AllowDuplicatePort;
    UINT8            TypeOfService;
    UINT8            TimeToLive;
    BOOLEAN          DoNotFragment;
    UINT32           ReceiveTimeout;
    UINT32           TransmitTimeout;
    BOOLEAN          UseDefaultAddress;
    EFI_IPv4_ADDRESS StationAddress;
    EFI_IPv4_ADDRESS SubnetMask;
    UINT16           StationPort;
    EFI_IPv4_ADDRESS RemoteAddress;
    UINT16           RemotePort;
} EFI_UDP4_CONFIG_DATA;

/* ---- UDP4 session data (UEFI 2.10 §30.2.6) — per-datagram src/dst override ---- */
typedef struct {
    EFI_IPv4_ADDRESS SourceAddress;
    UINT16           SourcePort;
    EFI_IPv4_ADDRESS DestinationAddress;
    UINT16           DestinationPort;
} EFI_UDP4_SESSION_DATA;

/* ---- Fragment table entry (shared shape with TCP4) ---- */
typedef struct {
    UINT32 FragmentLength;
    VOID  *FragmentBuffer;
} EFI_UDP4_FRAGMENT_DATA;

/* ---- Transmit data (UEFI 2.10 §30.2.6, EFI_UDP4_TRANSMIT_DATA) ----
 * UdpSessionData NULL → use the Configure()d RemoteAddress/RemotePort. */
typedef struct {
    EFI_UDP4_SESSION_DATA  *UdpSessionData;
    EFI_IPv4_ADDRESS       *GatewayAddress;
    UINT32                  DataLength;
    UINT32                  FragmentCount;
    EFI_UDP4_FRAGMENT_DATA  FragmentTable[1];
} EFI_UDP4_TRANSMIT_DATA;

/* ---- Receive data (UEFI 2.10 §30.2.7, EFI_UDP4_RECEIVE_DATA) ----
 * The firmware ALLOCATES this on completion; the app MUST SignalEvent(RecycleSignal)
 * when done to return the buffer to the driver. */
typedef struct {
    EFI_TIME                TimeStamp;
    EFI_EVENT               RecycleSignal;
    EFI_UDP4_SESSION_DATA   UdpSession;
    UINT32                  DataLength;
    UINT32                  FragmentCount;
    EFI_UDP4_FRAGMENT_DATA  FragmentTable[1];
} EFI_UDP4_RECEIVE_DATA;

/* ---- Completion token (UEFI 2.10 §30.2.2, EFI_UDP4_COMPLETION_TOKEN) ---- */
typedef struct {
    EFI_EVENT  Event;
    EFI_STATUS Status;
    union {
        EFI_UDP4_RECEIVE_DATA  *RxData;
        EFI_UDP4_TRANSMIT_DATA *TxData;
    } Packet;
} EFI_UDP4_COMPLETION_TOKEN;

/* ---- UDP4 protocol vtable (UEFI 2.10 §30.2) ----
 * Only Configure / Transmit / Receive / Poll are called by U-5; the rest are declared
 * so the vtable ABI matches the firmware's expectation. */
typedef struct _EFI_UDP4_PROTOCOL EFI_UDP4_PROTOCOL;

struct _EFI_UDP4_PROTOCOL {
    EFI_STATUS (EFIAPI *GetModeData)(EFI_UDP4_PROTOCOL     *This,
                                     EFI_UDP4_CONFIG_DATA  *Udp4ConfigData,
                                     VOID                  *Ip4ModeData,
                                     VOID                  *MnpConfigData,
                                     VOID                  *SnpModeData);
    EFI_STATUS (EFIAPI *Configure)(EFI_UDP4_PROTOCOL     *This,
                                   EFI_UDP4_CONFIG_DATA  *UdpConfigData);
    EFI_STATUS (EFIAPI *Groups)(EFI_UDP4_PROTOCOL *This,
                                BOOLEAN            JoinFlag,
                                EFI_IPv4_ADDRESS *MulticastAddress);
    EFI_STATUS (EFIAPI *Routes)(EFI_UDP4_PROTOCOL *This,
                                BOOLEAN            DeleteRoute,
                                EFI_IPv4_ADDRESS *SubnetAddress,
                                EFI_IPv4_ADDRESS *SubnetMask,
                                EFI_IPv4_ADDRESS *GatewayAddress);
    EFI_STATUS (EFIAPI *Transmit)(EFI_UDP4_PROTOCOL         *This,
                                  EFI_UDP4_COMPLETION_TOKEN *Token);
    EFI_STATUS (EFIAPI *Receive)(EFI_UDP4_PROTOCOL         *This,
                                 EFI_UDP4_COMPLETION_TOKEN *Token);
    EFI_STATUS (EFIAPI *Cancel)(EFI_UDP4_PROTOCOL         *This,
                                EFI_UDP4_COMPLETION_TOKEN *Token);
    EFI_STATUS (EFIAPI *Poll)(EFI_UDP4_PROTOCOL *This);
};

#endif /* SPIKE_EFI_UDP4_H */
