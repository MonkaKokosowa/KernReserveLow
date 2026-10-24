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
 * v4: only while macOS is starting. v3 changed the map for every os opencore started, and windows' boot loader
 * stops at the oem logo when the whole low ram is "reserved" (tested 2026-10-24). so this also wraps
 * gBS->StartImage and only changes the map while an apple boot loader is running: file name boot.efi or
 * tmbootpicker.efi, or a path with com.apple.recovery.boot in it, the same test opencore itself uses
 * (OcGetBootDevicePathType). opencore loads UEFI -> Drivers after it installed its own StartImage hooks, so this
 * wrapper is the outermost one and sees every start, also the images opencore loads itself. keep LoadEarly off.
 * driver argument "always" (UEFI -> Drivers -> Arguments) brings back the v3 behaviour for every os.
 *
 * two builds (edk2 targets):
 *  - RELEASE: just the hooks. no log file, no nvram writes, nothing on screen.
 *  - DEBUG: also saves what it did into an nvram variable at ExitBootServices and writes that into
 *    \KernReserveLow.log on the opencore partition on the next boot, so you can check it worked.
 */
#include <Uefi.h>
#include <Guid/FileInfo.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/DevicePath.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#define RANGE_LO  0x0ULL           /* yes, from 0. the first MB failed too. xnu uses its pages 0-2 (low globals,
                                       real mode trampoline) by fixed address, not through the memory map */
#define RANGE_HI  0x9F14000ULL      /* end of the free ram below the lowest kernel slide on this laptop (slide 80 = 0xA100000) */

#ifndef MDEPKG_NDEBUG
#define KRL_DEBUG 1
#endif

STATIC EFI_GET_MEMORY_MAP  mOrigGetMemoryMap;
STATIC EFI_IMAGE_START     mOrigStartImage;
STATIC UINTN               mAppleNest;   /* > 0 while an apple boot loader is running */
STATIC BOOLEAN             mAlways;      /* driver argument "always": v3 behaviour */

#ifdef KRL_DEBUG
#define MAX_MARK  16
#define NAME_LEN  48
#define RESULT_VAR_ATTRS (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS)

/* 5A1B0C3D-7E2F-4A61-9B8C-4D5E6F708192, made up for this driver */
STATIC EFI_GUID gResultGuid = { 0x5a1b0c3d, 0x7e2f, 0x4a61, { 0x9b, 0x8c, 0x4d, 0x5e, 0x6f, 0x70, 0x81, 0x92 } };

typedef struct {
  UINT32  Version;           /* 4 */
  UINT32  Calls;             /* GetMemoryMap calls that were changed */
  UINT32  Passed;            /* GetMemoryMap calls passed through untouched (no apple loader running) */
  UINT32  Marked;            /* descriptors changed in the last changed call */
  UINT64  Pages;             /* pages changed in the last changed call */
  UINT32  Skipped;           /* changed too, just didn't fit in the list below */
  UINT32  AppleStarts;       /* StartImage calls for an apple boot loader */
  UINT32  OtherStarts;       /* StartImage calls for anything else */
  UINT32  ArmedAtExit;       /* 1 = the map was being changed when boot services ended */
  UINT64  Start[MAX_MARK];
  UINT64  Count[MAX_MARK];
  CHAR16  LastApple[NAME_LEN];
  CHAR16  LastOther[NAME_LEN];
} RESULT;

STATIC RESULT     mResult;
STATIC EFI_EVENT  mExitEvent;
#endif

/* ------------------------------------------------------------------ which image is starting */

STATIC CHAR16
Lower (
  IN CHAR16  C
  )
{
  return (C >= L'A' && C <= L'Z') ? (CHAR16)(C - L'A' + L'a') : C;
}

/* case-insensitive: does Str[0..Len) contain the lower-case ascii Word */
STATIC BOOLEAN
Contains (
  IN CONST CHAR16  *Str,
  IN UINTN         Len,
  IN CONST CHAR16  *Word
  )
{
  UINTN  WordLen = StrLen (Word);
  UINTN  I, J;

  for (I = 0; I + WordLen <= Len; I++) {
    for (J = 0; J < WordLen && Lower (Str[I + J]) == Word[J]; J++) {
    }

    if (J == WordLen) {
      return TRUE;
    }
  }

  return FALSE;
}

/* last path component of the image's file path, plus whether any part names com.apple.recovery.boot */
STATIC BOOLEAN
IsAppleBootLoader (
  IN  EFI_HANDLE  ImageHandle,
  OUT CHAR16      *Name OPTIONAL,
  IN  UINTN       NameLen
  )
{
  EFI_LOADED_IMAGE_PROTOCOL  *Image;
  EFI_DEVICE_PATH_PROTOCOL   *Node;
  CONST CHAR16               *Path;
  CONST CHAR16               *Last = NULL;
  UINTN                      PathLen, LastLen = 0, I, Start;
  BOOLEAN                    Recovery = FALSE;

  if (Name != NULL && NameLen > 0) {
    Name[0] = 0;
  }

  if (EFI_ERROR (gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Image))
      || Image->FilePath == NULL) {
    return FALSE;
  }

  for (Node = Image->FilePath; !IsDevicePathEnd (Node); Node = NextDevicePathNode (Node)) {
    if (DevicePathType (Node) != MEDIA_DEVICE_PATH || DevicePathSubType (Node) != MEDIA_FILEPATH_DP
        || DevicePathNodeLength (Node) <= SIZE_OF_FILEPATH_DEVICE_PATH) {
      continue;
    }

    Path    = ((FILEPATH_DEVICE_PATH *)Node)->PathName;
    PathLen = (DevicePathNodeLength (Node) - SIZE_OF_FILEPATH_DEVICE_PATH) / sizeof (CHAR16);
    while (PathLen > 0 && Path[PathLen - 1] == 0) {
      PathLen--;
    }

    if (Contains (Path, PathLen, L"com.apple.recovery.boot")) {
      Recovery = TRUE;
    }

    for (Start = PathLen; Start > 0 && Path[Start - 1] != L'\\' && Path[Start - 1] != L'/'; Start--) {
    }

    if (Start < PathLen) {
      Last    = &Path[Start];
      LastLen = PathLen - Start;
    }
  }

  if (Last != NULL && Name != NULL && NameLen > 0) {
    for (I = 0; I < LastLen && I + 1 < NameLen; I++) {
      Name[I] = Last[I];
    }

    Name[I] = 0;
  }

  if (Recovery) {
    return TRUE;
  }

  return Last != NULL
         && ((LastLen == 8 && Contains (Last, LastLen, L"boot.efi"))
             || (LastLen == 16 && Contains (Last, LastLen, L"tmbootpicker.efi")));
}

STATIC
EFI_STATUS
EFIAPI
HookStartImage (
  IN  EFI_HANDLE  ImageHandle,
  OUT UINTN       *ExitDataSize,
  OUT CHAR16      **ExitData OPTIONAL
  )
{
  EFI_STATUS  Status;
  BOOLEAN     Apple;

 #ifdef KRL_DEBUG
  CHAR16  Name[NAME_LEN];

  Apple = IsAppleBootLoader (ImageHandle, Name, NAME_LEN);
  if (Apple) {
    mResult.AppleStarts++;
    CopyMem (mResult.LastApple, Name, sizeof (Name));
  } else {
    mResult.OtherStarts++;
    CopyMem (mResult.LastOther, Name, sizeof (Name));
  }

 #else
  Apple = IsAppleBootLoader (ImageHandle, NULL, 0);
 #endif

  if (Apple) {
    mAppleNest++;
  }

  Status = mOrigStartImage (ImageHandle, ExitDataSize, ExitData);

  /* boot.efi normally never comes back (ExitBootServices, kernel). if it does, the map is left alone again */
  if (Apple && mAppleNest > 0) {
    mAppleNest--;
  }

  return Status;
}

/* ------------------------------------------------------------------ the memory map */

STATIC
EFI_STATUS
EFIAPI
HookGetMemoryMap (
  IN OUT UINTN                  *MemoryMapSize,
  OUT    EFI_MEMORY_DESCRIPTOR  *MemoryMap,
  OUT    UINTN                  *MapKey,
  OUT    UINTN                  *DescriptorSize,
  OUT    UINT32                 *DescriptorVersion
  )
{
  EFI_STATUS             Status;
  UINT8                  *P;
  UINTN                  Off;
  EFI_MEMORY_DESCRIPTOR  *D;
  UINT64                 End;
  BOOLEAN                Usable;

 #ifdef KRL_DEBUG
  UINT32  Marked = 0, Skipped = 0;
  UINT64  Pages  = 0;
 #endif

  Status = mOrigGetMemoryMap (MemoryMapSize, MemoryMap, MapKey, DescriptorSize, DescriptorVersion);
  if (EFI_ERROR (Status) || MemoryMap == NULL || MemoryMapSize == NULL || DescriptorSize == NULL
      || *DescriptorSize < sizeof (EFI_MEMORY_DESCRIPTOR)) {
    return Status;
  }

  if (!mAlways && mAppleNest == 0) {
 #ifdef KRL_DEBUG
    mResult.Passed++;
 #endif
    return Status;
  }

  P = (UINT8 *)MemoryMap;
  for (Off = 0; Off + *DescriptorSize <= *MemoryMapSize; Off += *DescriptorSize) {
    D      = (EFI_MEMORY_DESCRIPTOR *)(P + Off);
    End    = D->PhysicalStart + D->NumberOfPages * EFI_PAGE_SIZE;
    Usable = D->Type == EfiConventionalMemory || D->Type == EfiBootServicesCode
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
  mResult.Marked  = Marked;
  mResult.Pages   = Pages;
  mResult.Skipped = Skipped;
 #endif
  return Status;
}

#ifdef KRL_DEBUG

STATIC
VOID
EFIAPI
OnExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  mResult.ArmedAtExit = (mAlways || mAppleNest > 0) ? 1 : 0;
  gRT->SetVariable (L"KernReserveLow", &gResultGuid, RESULT_VAR_ATTRS, sizeof (mResult), &mResult);
}

/* ------------------------------------------------------------------ the little text log */

STATIC CHAR16  mLog[4096];
STATIC UINTN   mLogLen;
STATIC CHAR16  mTmp[256];

STATIC
VOID
Append (
  IN CONST CHAR16  *S
  )
{
  while (*S != 0 && mLogLen + 1 < ARRAY_SIZE (mLog)) {
    mLog[mLogLen++] = *S++;
  }

  mLog[mLogLen] = 0;
}

#define LOG(...)  do { UnicodeSPrint (mTmp, sizeof (mTmp), __VA_ARGS__); Append (mTmp); Print (L"%s", mTmp); } while (0)

STATIC
VOID
FlushLog (
  VOID
  )
{
  EFI_HANDLE                       *Handles = NULL;
  UINTN                            Count    = 0;
  UINTN                            Index;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root, *Probe, *F;
  UINTN                            Bytes;

  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &Count, &Handles))) {
    return;
  }

  for (Index = 0; Index < Count; Index++) {
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs))
        || EFI_ERROR (Fs->OpenVolume (Fs, &Root))) {
      continue;
    }

    if (EFI_ERROR (Root->Open (Root, &Probe, L"\\EFI\\OC\\OpenCore.efi", EFI_FILE_MODE_READ, 0))) {
      Root->Close (Root);
      continue;
    }

    Probe->Close (Probe);
    if (!EFI_ERROR (Root->Open (Root, &F, L"\\KernReserveLow.log",
                                EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0))) {
      F->SetPosition (F, MAX_UINT64);     /* UEFI: this position means end of file */
      Bytes = mLogLen * sizeof (CHAR16);
      F->Write (F, &Bytes, mLog);
      F->Close (F);
    }

    Root->Close (Root);
    break;
  }

  FreePool (Handles);
}

#endif

/* ------------------------------------------------------------------ driver argument "always" */

STATIC
BOOLEAN
ArgumentAlways (
  IN EFI_HANDLE  ImageHandle
  )
{
  EFI_LOADED_IMAGE_PROTOCOL  *Self;
  CONST CHAR16               *Opt;

  if (EFI_ERROR (gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Self))
      || Self->LoadOptions == NULL || Self->LoadOptionsSize < sizeof (CHAR16)) {
    return FALSE;
  }

  Opt = (CONST CHAR16 *)Self->LoadOptions;
  return Contains (Opt, Self->LoadOptionsSize / sizeof (CHAR16), L"always");
}

EFI_STATUS
EFIAPI
KernReserveLowEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_TPL  OldTpl;
  UINT32   Crc;

 #ifdef KRL_DEBUG
  EFI_TIME    Now;
  RESULT      *Prev     = NULL;
  UINTN       PrevSize  = 0;
  UINT32      Index;
  EFI_STATUS  Status;
 #endif

  mAlways = ArgumentAlways (ImageHandle);

 #ifdef KRL_DEBUG
  if (!EFI_ERROR (gRT->GetTime (&Now, NULL))) {
    LOG (
      L"=== KernReserveLow v4 (retype to Reserved while macOS starts%s) load %04d-%02d-%02d %02d:%02d:%02d, range 0x%lx-0x%lx\r\n",
      mAlways ? L", ALWAYS" : L"",
      Now.Year,
      Now.Month,
      Now.Day,
      Now.Hour,
      Now.Minute,
      Now.Second,
      RANGE_LO,
      RANGE_HI
      );
  }

  if (!EFI_ERROR (GetVariable2 (L"KernReserveLow", &gResultGuid, (VOID **)&Prev, &PrevSize))
      && Prev != NULL && PrevSize == sizeof (RESULT) && Prev->Version == 4) {
    LOG (
      L"previous boot: started %d apple loader(s) (last %s), %d other image(s) (last %s); armed at exit: %s\r\n",
      Prev->AppleStarts,
      Prev->LastApple,
      Prev->OtherStarts,
      Prev->LastOther,
      Prev->ArmedAtExit ? L"yes" : L"no"
      );
    LOG (
      L"previous boot: %d GetMemoryMap calls changed, %d passed through; last changed call retyped %d descriptors / %ld pages (%ld MB), %d not listed\r\n",
      Prev->Calls,
      Prev->Passed,
      Prev->Marked,
      Prev->Pages,
      Prev->Pages / 256,
      Prev->Skipped
      );
    for (Index = 0; Index < Prev->Marked && Index < MAX_MARK; Index++) {
      LOG (L"  0x%lx - 0x%lx\r\n", Prev->Start[Index], Prev->Start[Index] + Prev->Count[Index] * EFI_PAGE_SIZE);
    }
  } else {
    LOG (L"previous boot: no v4 result variable\r\n");
  }

  if (Prev != NULL) {
    FreePool (Prev);
  }

  gRT->SetVariable (L"KernReserveLow", &gResultGuid, RESULT_VAR_ATTRS, 0, NULL);
  mResult.Version = 4;
 #endif

  OldTpl            = gBS->RaiseTPL (TPL_HIGH_LEVEL);
  mOrigGetMemoryMap = gBS->GetMemoryMap;
  gBS->GetMemoryMap = HookGetMemoryMap;
  mOrigStartImage   = gBS->StartImage;
  gBS->StartImage   = HookStartImage;
  gBS->Hdr.CRC32    = 0;
  gBS->RestoreTPL (OldTpl);
  Crc = 0;
  gBS->CalculateCrc32 (gBS, gBS->Hdr.HeaderSize, &Crc);   /* a boot service: only after the TPL is back down */
  gBS->Hdr.CRC32 = Crc;

 #ifdef KRL_DEBUG
  Status = gBS->CreateEvent (EVT_SIGNAL_EXIT_BOOT_SERVICES, TPL_NOTIFY, OnExitBootServices, NULL, &mExitEvent);
  LOG (L"GetMemoryMap + StartImage hooks installed, ExitBootServices event: %r\r\n", Status);
  FlushLog ();
 #endif
  return EFI_SUCCESS;
}
