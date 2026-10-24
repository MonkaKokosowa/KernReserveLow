/*
 * KernReserveLow.efi
 *
 * tiny uefi driver for opencore (UEFI -> Drivers) that keeps macOS from using the lowest ~159 MB of ram.
 *
 * why: on my lenovo loq 15arp9 the amd usb controllers throw a Host System Error (macOS logs
 * "hardwareExceptionThreadCallGated: 0x00000020 (system error)") the moment a usb transfer buffer lands
 * in physical ram below about 0x9F14000. macOS only hands out that ram late, when free memory is running
 * low, so usb drives (and my usb system ssd!!) randomly dropped out after a few gigs of reading or when
 * apps ate all the memory. i caught the actual buffer addresses with a debug kext, the lowest ones were
 * 0x78000, 0x138000, 18-83 MB and 130-150 MB. everything from ~255 MB up worked fine.
 *
 * how: it wraps gBS->GetMemoryMap and changes every free descriptor (Conventional, BootServicesCode,
 * BootServicesData) that sits completely inside [RANGE_LO, RANGE_HI) to EfiReservedMemoryType. xnu never
 * puts reserved ranges into its page pool, so that ram never becomes a usb buffer. nothing actually gets
 * allocated in the firmware though, so boot.efi can still use that memory while it loads the kernel.
 * (opencore's ReservedMemory does allocate it, and boot.efi then dies with EB|STOP 0x16. ask me how i know.)
 *
 * there are two builds of this file:
 *  - RELEASE: just the hook. no log file, no nvram writes, nothing on screen.
 *  - DEBUG (built with -DKRL_DEBUG): also saves what it did into an nvram variable at ExitBootServices and
 *    writes that into \KernReserveLow.log on the opencore partition on the next boot, so you can check it worked.
 */
#include <efi.h>
#include <efilib.h>

#define RANGE_LO  0x0ULL           /* yes, from 0. the first MB failed too. xnu uses its pages 0-2 (low globals,
                                       real mode trampoline) by fixed address, not through the memory map */
#define RANGE_HI  0x9F14000ULL      /* end of the free ram below the lowest kernel slide on this laptop (slide 80 = 0xA100000) */

#ifdef KRL_DEBUG
#define MAX_MARK  16
#define RESULT_VAR_ATTRS (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS)

/* 5A1B0C3D-7E2F-4A61-9B8C-4D5E6F708192, made up for this driver */
static EFI_GUID gResultGuid = { 0x5a1b0c3d, 0x7e2f, 0x4a61, { 0x9b, 0x8c, 0x4d, 0x5e, 0x6f, 0x70, 0x81, 0x92 } };

typedef struct {
    UINT32 Calls;           /* how many GetMemoryMap calls went through */
    UINT32 Marked;          /* descriptors changed in the last call */
    UINT64 Pages;           /* pages changed in the last call */
    UINT32 Skipped;         /* changed too, just didn't fit in the list below */
    UINT32 Pad;
    UINT64 Start[MAX_MARK];
    UINT64 Count[MAX_MARK];
} RESULT;

static RESULT mResult;
#endif
static EFI_GET_MEMORY_MAP mOrigGetMemoryMap;
#ifdef KRL_DEBUG
static EFI_EVENT mExitEvent;
#endif

static EFI_STATUS EFIAPI HookGetMemoryMap(UINTN *MemoryMapSize, EFI_MEMORY_DESCRIPTOR *MemoryMap, UINTN *MapKey,
                                          UINTN *DescriptorSize, UINT32 *DescriptorVersion)
{
    EFI_STATUS Status = uefi_call_wrapper(mOrigGetMemoryMap, 5, MemoryMapSize, MemoryMap, MapKey, DescriptorSize,
                                          DescriptorVersion);
    if (EFI_ERROR(Status) || MemoryMap == NULL || MemoryMapSize == NULL || DescriptorSize == NULL
        || *DescriptorSize < sizeof(EFI_MEMORY_DESCRIPTOR)) {
        return Status;
    }
#ifdef KRL_DEBUG
    UINT32 Marked = 0, Skipped = 0;
    UINT64 Pages = 0;
#endif
    UINT8 *P = (UINT8 *)MemoryMap;
    for (UINTN Off = 0; Off + *DescriptorSize <= *MemoryMapSize; Off += *DescriptorSize) {
        EFI_MEMORY_DESCRIPTOR *D = (EFI_MEMORY_DESCRIPTOR *)(P + Off);
        UINT64 End = D->PhysicalStart + D->NumberOfPages * 4096ULL;
        BOOLEAN Usable = D->Type == EfiConventionalMemory || D->Type == EfiBootServicesCode
                         || D->Type == EfiBootServicesData;
        if (!Usable || D->PhysicalStart < RANGE_LO || End > RANGE_HI || D->NumberOfPages == 0) {
            continue;
        }
        D->Type = EfiReservedMemoryType;
#ifdef KRL_DEBUG
        if (Marked < MAX_MARK) {
            mResult.Start[Marked] = D->PhysicalStart;
            mResult.Count[Marked] = D->NumberOfPages;
        } else {
            Skipped++;      /* still changed, just not written down */
        }
        Marked++;
        Pages += D->NumberOfPages;
#endif
    }
#ifdef KRL_DEBUG
    mResult.Calls++;
    mResult.Marked = Marked;
    mResult.Pages = Pages;
    mResult.Skipped = Skipped;
#endif
    return Status;
}

#ifdef KRL_DEBUG

static VOID EFIAPI OnExitBootServices(EFI_EVENT Event, VOID *Context)
{
    (VOID)Event;
    (VOID)Context;
    uefi_call_wrapper(RT->SetVariable, 5, L"KernReserveLow", &gResultGuid, RESULT_VAR_ATTRS, sizeof(mResult), &mResult);
}

/* ------------------------------------------------------------------ the little text log */

static CHAR16 mLog[4096];
static UINTN  mLogLen;
static CHAR16 mTmp[256];

static VOID Append(CONST CHAR16 *S)
{
    while (*S != 0 && mLogLen + 1 < sizeof(mLog) / sizeof(mLog[0])) {
        mLog[mLogLen++] = *S++;
    }
    mLog[mLogLen] = 0;
}

#define LOG(...) do { UnicodeSPrint(mTmp, sizeof(mTmp), __VA_ARGS__); Append(mTmp); Print(L"%s", mTmp); } while (0)

static VOID FlushLog(VOID)
{
    EFI_HANDLE *Handles = NULL;
    UINTN Count = 0;
    if (EFI_ERROR(uefi_call_wrapper(BS->LocateHandleBuffer, 5, ByProtocol, &FileSystemProtocol, NULL, &Count, &Handles))) {
        return;
    }
    for (UINTN i = 0; i < Count; i++) {
        EFI_FILE_HANDLE Root = LibOpenRoot(Handles[i]);
        if (Root == NULL) {
            continue;
        }
        EFI_FILE_HANDLE Probe = NULL;
        if (EFI_ERROR(uefi_call_wrapper(Root->Open, 5, Root, &Probe, L"\\EFI\\OC\\OpenCore.efi", EFI_FILE_MODE_READ, 0))) {
            uefi_call_wrapper(Root->Close, 1, Root);
            continue;
        }
        uefi_call_wrapper(Probe->Close, 1, Probe);
        EFI_FILE_HANDLE F = NULL;
        if (!EFI_ERROR(uefi_call_wrapper(Root->Open, 5, Root, &F, L"\\KernReserveLow.log",
                                         EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0))) {
            EFI_FILE_INFO *Info = LibFileInfo(F);
            if (Info != NULL) {
                uefi_call_wrapper(F->SetPosition, 2, F, Info->FileSize);
                FreePool(Info);
            }
            UINTN Bytes = mLogLen * sizeof(CHAR16);
            uefi_call_wrapper(F->Write, 3, F, &Bytes, mLog);
            uefi_call_wrapper(F->Close, 1, F);
        }
        uefi_call_wrapper(Root->Close, 1, Root);
        break;
    }
    FreePool(Handles);
}
#endif

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    InitializeLib(ImageHandle, SystemTable);

#ifdef KRL_DEBUG
    EFI_TIME Now;
    if (!EFI_ERROR(uefi_call_wrapper(RT->GetTime, 2, &Now, NULL))) {
        LOG(L"=== KernReserveLow v3 (retype to Reserved, from 0) load %04d-%02d-%02d %02d:%02d:%02d, range 0x%lx-0x%lx\r\n", Now.Year, Now.Month,
            Now.Day, Now.Hour, Now.Minute, Now.Second, RANGE_LO, RANGE_HI);
    }

    UINTN PrevSize = 0;
    RESULT *Prev = LibGetVariableAndSize(L"KernReserveLow", &gResultGuid, &PrevSize);
    if (Prev != NULL && PrevSize >= sizeof(RESULT)) {
        LOG(L"previous boot: %d GetMemoryMap calls, last call retyped %d descriptors / %ld pages (%ld MB), %d not listed\r\n",
            Prev->Calls, Prev->Marked, Prev->Pages, Prev->Pages / 256, Prev->Skipped);
        for (UINT32 i = 0; i < Prev->Marked && i < MAX_MARK; i++) {
            LOG(L"  0x%lx - 0x%lx\r\n", Prev->Start[i], Prev->Start[i] + Prev->Count[i] * 4096);
        }
    } else {
        LOG(L"previous boot: no result variable\r\n");
    }
    if (Prev != NULL) {
        FreePool(Prev);
    }
    uefi_call_wrapper(RT->SetVariable, 5, L"KernReserveLow", &gResultGuid, RESULT_VAR_ATTRS, 0, NULL);
#endif

    EFI_TPL OldTpl = uefi_call_wrapper(BS->RaiseTPL, 1, TPL_HIGH_LEVEL);
    mOrigGetMemoryMap = BS->GetMemoryMap;
    BS->GetMemoryMap = HookGetMemoryMap;
    BS->Hdr.CRC32 = 0;
    UINT32 Crc = 0;
    uefi_call_wrapper(BS->RestoreTPL, 1, OldTpl);
    uefi_call_wrapper(BS->CalculateCrc32, 3, BS, BS->Hdr.HeaderSize, &Crc);
    BS->Hdr.CRC32 = Crc;

#ifdef KRL_DEBUG
    EFI_STATUS Status = uefi_call_wrapper(BS->CreateEvent, 5, EVT_SIGNAL_EXIT_BOOT_SERVICES, TPL_NOTIFY,
                                          OnExitBootServices, NULL, &mExitEvent);
    LOG(L"GetMemoryMap hook installed, ExitBootServices event: %r\r\n", Status);
    FlushLog();
#endif
    return EFI_SUCCESS;
}
