/*
 * efi_tcp4.h — minimal EFI_TCP4 + ServiceBinding protocol surface for the U-0 spike.
 *
 * Vendored from the UEFI Specification 2.10, sections:
 *   - 28.1  EFI_SERVICE_BINDING_PROTOCOL           (CreateChild / DestroyChild)
 *   - 28.2  EFI_MANAGED_NETWORK_PROTOCOL (types)
 *   - 28.3  EFI_IP4 addressing types
 *   - 28.5  EFI_TCP4_PROTOCOL                       (Configure/Connect/Transmit/Receive/...)
 *
 * gnu-efi's TCP4 protocol coverage is incomplete, so the TCP4/ServiceBinding vtables +
 * token structs are defined here per spec. This is standard practice for a UEFI spike.
 *
 * Depends on efi_min.h already being included (for EFI_STATUS, EFI_EVENT, EFI_GUID,
 * EFI_HANDLE, EFIAPI, BOOLEAN, UINT8/16/32, UINTN, VOID).
 */
#ifndef SPIKE_EFI_TCP4_H
#define SPIKE_EFI_TCP4_H

/* ---- GUIDs (UEFI 2.10, Appendix A / protocol sections) ---- */

/* EFI_TCP4_SERVICE_BINDING_PROTOCOL_GUID  00720665-67EB-4a99-BAF7-D3C33A1C7CC9 */
#define EFI_TCP4_SERVICE_BINDING_PROTOCOL_GUID \
    { 0x00720665, 0x67EB, 0x4a99, {0xBA, 0xF7, 0xD3, 0xC3, 0x3A, 0x1C, 0x7C, 0xC9} }

/* EFI_TCP4_PROTOCOL_GUID                   65530BC7-A359-410f-B010-5AADC7EC2B62 */
#define EFI_TCP4_PROTOCOL_GUID \
    { 0x65530BC7, 0xA359, 0x410f, {0xB0, 0x10, 0x5A, 0xAD, 0xC7, 0xEC, 0x2B, 0x62} }

/* ---- Address types (UEFI 2.10 §28.3.1) ---- */

typedef struct {
    UINT8 Addr[4];
} EFI_IPv4_ADDRESS;

/* ---- Service Binding protocol (UEFI 2.10 §28.1) ---- */

typedef struct _EFI_SERVICE_BINDING_PROTOCOL EFI_SERVICE_BINDING_PROTOCOL;

struct _EFI_SERVICE_BINDING_PROTOCOL {
    EFI_STATUS (EFIAPI *CreateChild)(EFI_SERVICE_BINDING_PROTOCOL *This,
                                     EFI_HANDLE                   *ChildHandle);
    EFI_STATUS (EFIAPI *DestroyChild)(EFI_SERVICE_BINDING_PROTOCOL *This,
                                      EFI_HANDLE                    ChildHandle);
};

/* ---- TCP4 config / state (UEFI 2.10 §28.5.1) ---- */

typedef struct {
    BOOLEAN          UseDefaultAddress;
    EFI_IPv4_ADDRESS StationAddress;
    EFI_IPv4_ADDRESS SubnetMask;
    UINT16           StationPort;
    EFI_IPv4_ADDRESS RemoteAddress;
    UINT16           RemotePort;
    BOOLEAN          ActiveFlag;
} EFI_TCP4_ACCESS_POINT;

typedef struct {
    UINT32           ReceiveBufferSize;
    UINT32           SendBufferSize;
    UINT32           MaxSynBackLog;
    UINT32           ConnectionTimeout;
    UINT32           DataRetries;
    UINT32           FinTimeout;
    UINT32           TimeWaitTimeout;
    UINT32           KeepAliveProbes;
    UINT32           KeepAliveTime;
    UINT32           KeepAliveInterval;
    BOOLEAN          EnableNagle;
    BOOLEAN          EnableTimeStamp;
    BOOLEAN          EnableWindowScaling;
    BOOLEAN          EnableSelectiveAck;
    BOOLEAN          EnablePathMtuDiscovery;
} EFI_TCP4_OPTION;

typedef struct {
    UINT8                 TypeOfService;
    UINT8                 TimeToLive;
    EFI_TCP4_ACCESS_POINT AccessPoint;
    EFI_TCP4_OPTION      *ControlOption;
} EFI_TCP4_CONFIG_DATA;

/* ---- Completion token (UEFI 2.10 §28.5.2) ---- */

typedef struct {
    EFI_EVENT  Event;
    EFI_STATUS Status;
} EFI_TCP4_COMPLETION_TOKEN;

typedef struct {
    EFI_TCP4_COMPLETION_TOKEN CompletionToken;
} EFI_TCP4_CONNECTION_TOKEN;

/* Fragment table entries (§28.5.5 Transmit / §28.5.6 Receive) */
typedef struct {
    UINT32 FragmentLength;
    VOID  *FragmentBuffer;
} EFI_TCP4_FRAGMENT_DATA;

typedef struct {
    BOOLEAN                Push;
    BOOLEAN                Urgent;
    UINT32                 DataLength;
    UINT32                 FragmentCount;
    EFI_TCP4_FRAGMENT_DATA FragmentTable[1];
} EFI_TCP4_TRANSMIT_DATA;

typedef struct {
    BOOLEAN                UrgentFlag;
    UINT32                 DataLength;
    UINT32                 FragmentCount;
    EFI_TCP4_FRAGMENT_DATA FragmentTable[1];
} EFI_TCP4_RECEIVE_DATA;

typedef struct {
    EFI_TCP4_COMPLETION_TOKEN CompletionToken;
    union {
        EFI_TCP4_RECEIVE_DATA  *RxData;
        EFI_TCP4_TRANSMIT_DATA *TxData;
    } Packet;
} EFI_TCP4_IO_TOKEN;

typedef struct {
    EFI_TCP4_COMPLETION_TOKEN CompletionToken;
    BOOLEAN                   AbortOnClose;
} EFI_TCP4_CLOSE_TOKEN;

/* ---- TCP4 protocol vtable (UEFI 2.10 §28.5) ---- */

typedef struct _EFI_TCP4_PROTOCOL EFI_TCP4_PROTOCOL;

/* GetModeData: full form has 6 out params; we only care about ConfigData here, but
 * declare the full signature so the ABI matches the firmware's expectation. */
struct _EFI_TCP4_PROTOCOL {
    EFI_STATUS (EFIAPI *GetModeData)(EFI_TCP4_PROTOCOL     *This,
                                     VOID                  *Tcp4State,        /* EFI_TCP4_CONNECTION_STATE* */
                                     EFI_TCP4_CONFIG_DATA  *Tcp4ConfigData,
                                     VOID                  *Ip4ModeData,
                                     VOID                  *MnpConfigData,
                                     VOID                  *SnpModeData);
    EFI_STATUS (EFIAPI *Configure)(EFI_TCP4_PROTOCOL     *This,
                                   EFI_TCP4_CONFIG_DATA  *TcpConfigData);
    EFI_STATUS (EFIAPI *Routes)(EFI_TCP4_PROTOCOL *This,
                                BOOLEAN            DeleteRoute,
                                EFI_IPv4_ADDRESS  *SubnetAddress,
                                EFI_IPv4_ADDRESS  *SubnetMask,
                                EFI_IPv4_ADDRESS  *GatewayAddress);
    EFI_STATUS (EFIAPI *Connect)(EFI_TCP4_PROTOCOL         *This,
                                 EFI_TCP4_CONNECTION_TOKEN *ConnectionToken);
    EFI_STATUS (EFIAPI *Accept)(EFI_TCP4_PROTOCOL       *This,
                                VOID                    *ListenToken);
    EFI_STATUS (EFIAPI *Transmit)(EFI_TCP4_PROTOCOL *This,
                                  EFI_TCP4_IO_TOKEN *Token);
    EFI_STATUS (EFIAPI *Receive)(EFI_TCP4_PROTOCOL *This,
                                 EFI_TCP4_IO_TOKEN *Token);
    EFI_STATUS (EFIAPI *Close)(EFI_TCP4_PROTOCOL    *This,
                               EFI_TCP4_CLOSE_TOKEN *CloseToken);
    EFI_STATUS (EFIAPI *Cancel)(EFI_TCP4_PROTOCOL         *This,
                                EFI_TCP4_COMPLETION_TOKEN *Token);
    EFI_STATUS (EFIAPI *Poll)(EFI_TCP4_PROTOCOL *This);
};

#endif /* SPIKE_EFI_TCP4_H */
