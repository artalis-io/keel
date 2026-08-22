/*
 * efi_min.h — self-contained minimal UEFI base types + the boot/console services the
 * U-0 spike uses. Vendored from the UEFI Specification 2.10 (§2 data types, §4 system
 * table, §7 boot services, §12.4 simple text output). Deliberately tiny: only what
 * tcp4_get.c needs, so the spike builds under the clang/lld freestanding toolchain
 * (x86_64-unknown-windows PE) with NO gnu-efi runtime dependency.
 *
 * EFIAPI == Microsoft x64 calling convention (UEFI ABI). clang honors it via __ms_abi__.
 */
#ifndef SPIKE_EFI_MIN_H
#define SPIKE_EFI_MIN_H

#include <stdint.h>
#include <stddef.h>

typedef uint8_t   BOOLEAN;
typedef int64_t   INTN;
typedef uint64_t  UINTN;
typedef uint8_t   UINT8;
typedef uint16_t  UINT16;
typedef uint32_t  UINT32;
typedef uint64_t  UINT64;
typedef int8_t    INT8;
typedef int16_t   INT16;
typedef int32_t   INT32;
typedef int64_t   INT64;
typedef uint16_t  CHAR16;
typedef char      CHAR8;
typedef void      VOID;

typedef UINTN     EFI_STATUS;
typedef VOID     *EFI_HANDLE;
typedef VOID     *EFI_EVENT;

#define TRUE  ((BOOLEAN)1)
#define FALSE ((BOOLEAN)0)
#ifndef NULL
#define NULL ((void *)0)
#endif

/* EFIAPI: the UEFI calling convention. x86_64 UEFI uses the MS x64 convention
 * (ms_abi); aarch64 UEFI uses AAPCS (no attribute). ms_abi is an x86-only
 * attribute — applying it elsewhere (e.g. a native aarch64 host compiling the
 * pure-mapping unit tests) is "ignored" and trips -Werror=attributes, so gate
 * it on the target arch. */
#if defined(__x86_64__)
#define EFIAPI __attribute__((ms_abi))
#else
#define EFIAPI
#endif

/* High bit set == error (64-bit). */
#define EFI_ERROR_BIT     ((UINTN)1 << 63)
#define EFIERR(a)         (EFI_ERROR_BIT | (a))
#define EFI_ERROR(s)      (((INTN)(UINTN)(s)) < 0)

#define EFI_SUCCESS            0
#define EFI_LOAD_ERROR         EFIERR(1)
#define EFI_INVALID_PARAMETER  EFIERR(2)
#define EFI_UNSUPPORTED        EFIERR(3)
#define EFI_NOT_READY          EFIERR(6)
#define EFI_DEVICE_ERROR       EFIERR(7)
#define EFI_OUT_OF_RESOURCES   EFIERR(9)
#define EFI_TIMEOUT            EFIERR(18)
#define EFI_NOT_FOUND          EFIERR(14)
#define EFI_ACCESS_DENIED      EFIERR(15)
#define EFI_NO_MAPPING         EFIERR(17)
#define EFI_ABORTED            EFIERR(21)
#define EFI_CONNECTION_FIN     EFIERR(104)
#define EFI_CONNECTION_RESET   EFIERR(102)
#define EFI_CONNECTION_REFUSED EFIERR(105)

typedef struct {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8  Data4[8];
} EFI_GUID;

/* ---- Event / TPL constants (UEFI 2.10 §7.1) ---- */
#define EVT_NOTIFY_WAIT   0x00000100
#define EVT_NOTIFY_SIGNAL 0x00000200
#define EVT_TIMER         0x80000000
/* Fires once when ExitBootServices() is called (UEFI 2.10 §7.1). The notify runs
 * while boot services are still momentarily usable; keep it minimal (a flag write). */
#define EVT_SIGNAL_EXIT_BOOT_SERVICES 0x00000201
#define TPL_APPLICATION   4
#define TPL_CALLBACK      8
#define TPL_NOTIFY        16

/* ---- OpenProtocol attributes (UEFI 2.10 §7.3) ---- */
#define EFI_OPEN_PROTOCOL_GET_PROTOCOL  0x00000002
#define EFI_OPEN_PROTOCOL_BY_DRIVER     0x00000010
#define EFI_OPEN_PROTOCOL_EXCLUSIVE     0x00000020

typedef enum { AllHandles, ByRegisterNotify, ByProtocol } EFI_LOCATE_SEARCH_TYPE;
typedef enum { TimerCancel, TimerPeriodic, TimerRelative } EFI_TIMER_DELAY;

/* ---- Simple Text Output (UEFI 2.10 §12.4) ---- */
typedef struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    VOID       *Reset;
    EFI_STATUS (EFIAPI *OutputString)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String);
    VOID       *TestString;
    VOID       *QueryMode;
    VOID       *SetMode;
    VOID       *SetAttribute;
    VOID       *ClearScreen;
    VOID       *SetCursorPosition;
    VOID       *EnableCursor;
    VOID       *Mode;
};

/* ---- Boot Services (UEFI 2.10 §4.4, partial — only what we call) ---- */
typedef struct {
    /* EFI_TABLE_HEADER (24 bytes) */
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;

    /* Task Priority Services */
    VOID  *RaiseTPL;
    VOID  *RestoreTPL;

    /* Memory Services */
    VOID  *AllocatePages;
    VOID  *FreePages;
    VOID  *GetMemoryMap;
    EFI_STATUS (EFIAPI *AllocatePool)(UINTN PoolType, UINTN Size, VOID **Buffer);
    EFI_STATUS (EFIAPI *FreePool)(VOID *Buffer);

    /* Event & Timer Services */
    EFI_STATUS (EFIAPI *CreateEvent)(UINT32 Type, UINTN NotifyTpl,
                                     VOID *NotifyFunction, VOID *NotifyContext,
                                     EFI_EVENT *Event);
    EFI_STATUS (EFIAPI *SetTimer)(EFI_EVENT Event, EFI_TIMER_DELAY Type, UINT64 TriggerTime);
    EFI_STATUS (EFIAPI *WaitForEvent)(UINTN NumberOfEvents, EFI_EVENT *Event, UINTN *Index);
    EFI_STATUS (EFIAPI *SignalEvent)(EFI_EVENT Event);
    EFI_STATUS (EFIAPI *CloseEvent)(EFI_EVENT Event);
    EFI_STATUS (EFIAPI *CheckEvent)(EFI_EVENT Event);

    /* Protocol Handler Services */
    VOID  *InstallProtocolInterface;
    VOID  *ReinstallProtocolInterface;
    VOID  *UninstallProtocolInterface;
    EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE Handle, EFI_GUID *Protocol, VOID **Interface);
    VOID  *Reserved2;
    VOID  *RegisterProtocolNotify;
    VOID  *LocateHandle;
    VOID  *LocateDevicePath;
    VOID  *InstallConfigurationTable;

    /* Image Services */
    VOID  *LoadImage;
    VOID  *StartImage;
    VOID  *Exit;
    VOID  *UnloadImage;
    VOID  *ExitBootServices;

    /* Misc Services */
    VOID  *GetNextMonotonicCount;
    EFI_STATUS (EFIAPI *Stall)(UINTN Microseconds);
    VOID  *SetWatchdogTimer;

    /* DriverSupport Services */
    VOID  *ConnectController;
    VOID  *DisconnectController;

    /* Open and Close Protocol Services */
    EFI_STATUS (EFIAPI *OpenProtocol)(EFI_HANDLE Handle, EFI_GUID *Protocol,
                                      VOID **Interface, EFI_HANDLE AgentHandle,
                                      EFI_HANDLE ControllerHandle, UINT32 Attributes);
    EFI_STATUS (EFIAPI *CloseProtocol)(EFI_HANDLE Handle, EFI_GUID *Protocol,
                                       EFI_HANDLE AgentHandle, EFI_HANDLE ControllerHandle);
    VOID  *OpenProtocolInformation;

    /* Library Services */
    VOID  *ProtocolsPerHandle;
    EFI_STATUS (EFIAPI *LocateHandleBuffer)(EFI_LOCATE_SEARCH_TYPE SearchType,
                                            EFI_GUID *Protocol, VOID *SearchKey,
                                            UINTN *NoHandles, EFI_HANDLE **Buffer);
    EFI_STATUS (EFIAPI *LocateProtocol)(EFI_GUID *Protocol, VOID *Registration,
                                        VOID **Interface);
    VOID  *InstallMultipleProtocolInterfaces;
    VOID  *UninstallMultipleProtocolInterfaces;

    /* 32-bit CRC Services */
    VOID  *CalculateCrc32;

    /* Misc Services */
    VOID  *CopyMem;
    VOID  *SetMem;
    VOID  *CreateEventEx;
} EFI_BOOT_SERVICES;

/* Memory pool types for AllocatePool */
#define EfiLoaderData 2

/* ---- EFI_TIME (UEFI 2.10 §8.3) — wall-clock, for cert validity-time (GetTime) ---- */
typedef struct {
    UINT16 Year;        /* 1900 .. 9999 */
    UINT8  Month;       /* 1 .. 12 */
    UINT8  Day;         /* 1 .. 31 */
    UINT8  Hour;        /* 0 .. 23 */
    UINT8  Minute;      /* 0 .. 59 */
    UINT8  Second;      /* 0 .. 59 */
    UINT8  Pad1;
    UINT32 Nanosecond;  /* 0 .. 999,999,999 */
    INT16  TimeZone;    /* -1440 .. 1440 or EFI_UNSPECIFIED_TIMEZONE */
    UINT8  Daylight;
    UINT8  Pad2;
} EFI_TIME;

#define EFI_UNSPECIFIED_TIMEZONE 0x07FF   /* GetTime returned local/unknown TZ */

typedef struct {                          /* §8.3 — opaque here (we pass NULL) */
    UINT32  Resolution;
    UINT32  Accuracy;
    BOOLEAN SetsToZero;
} EFI_TIME_CAPABILITIES;

/* ResetSystem ResetType (UEFI 2.10 §8.5.1). EfiResetShutdown powers the platform off. */
typedef enum {
    EfiResetCold,
    EfiResetWarm,
    EfiResetShutdown,
    EfiResetPlatformSpecific
} EFI_RESET_TYPE;

/* ---- Runtime Services (UEFI 2.10 §8) — GetTime + ResetSystem are modelled at their spec
 * offsets. GetTime is valid both before AND after ExitBootServices (the cert-validity clock
 * source). ResetSystem is the 11th runtime function (offset 104 on x64: the 24-byte
 * EFI_TABLE_HEADER + 10 preceding pointers); the intervening functions are opaque placeholders
 * ONLY to land ResetSystem at the correct offset for a real-firmware call — never dereferenced. */
typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;

    EFI_STATUS (EFIAPI *GetTime)(EFI_TIME *Time, EFI_TIME_CAPABILITIES *Capabilities);
    VOID *SetTime;
    VOID *GetWakeupTime;
    VOID *SetWakeupTime;
    VOID *SetVirtualAddressMap;
    VOID *ConvertPointer;
    VOID *GetVariable;
    VOID *GetNextVariableName;
    VOID *SetVariable;
    VOID *GetNextHighMonotonicCount;
    EFI_STATUS (EFIAPI *ResetSystem)(EFI_RESET_TYPE ResetType, EFI_STATUS ResetStatus,
                                     UINTN DataSize, VOID *ResetData);
} EFI_RUNTIME_SERVICES;

/* ---- System Table (UEFI 2.10 §4.3, partial) ---- */
typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;

    CHAR16                          *FirmwareVendor;
    UINT32                           FirmwareRevision;
    EFI_HANDLE                       ConsoleInHandle;
    VOID                            *ConIn;
    EFI_HANDLE                       ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE                       StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    EFI_RUNTIME_SERVICES            *RuntimeServices;
    EFI_BOOT_SERVICES               *BootServices;
    UINTN                            NumberOfTableEntries;
    VOID                            *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* Local ZeroMem (no libc). */
static inline void ZeroMem(void *p, UINTN n) {
    volatile UINT8 *b = (volatile UINT8 *)p;
    while (n--) *b++ = 0;
}

#endif /* SPIKE_EFI_MIN_H */
