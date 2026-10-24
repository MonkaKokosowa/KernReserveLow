## @file
#  KernReserveLowPkg: builds KernReserveLow.efi with EDK2 (acidanthera audk) on macOS (XCODE5) or Linux (GCC/CLANGPDB).
#  Build: ./build_krl.tool   (uses acidanthera/ocbuild efibuild.sh, like OpenCorePkg)
##
[Defines]
  PLATFORM_NAME           = KernReserveLowPkg
  PLATFORM_GUID           = 5A1B0C3D-7E2F-4A61-9B8C-4D5E6F708195
  PLATFORM_VERSION        = 1.1
  SUPPORTED_ARCHITECTURES = X64
  BUILD_TARGETS           = RELEASE|DEBUG|NOOPT
  SKUID_IDENTIFIER        = DEFAULT
  DSC_SPECIFICATION       = 0x00010005
  OUTPUT_DIRECTORY        = Build/KernReserveLowPkg

!include MdePkg/MdeLibs.dsc.inc   # edk2 defaults (StackCheckLib, CompilerIntrinsicsLib, RegisterFilterLib), as OpenCorePkg

[LibraryClasses]
  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
  DebugLib|MdePkg/Library/BaseDebugLibNull/BaseDebugLibNull.inf
  DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
  UefiBootServicesTableLib|MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
  UefiDriverEntryPoint|MdePkg/Library/UefiDriverEntryPoint/UefiDriverEntryPoint.inf
  UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
  UefiRuntimeServicesTableLib|MdePkg/Library/UefiRuntimeServicesTableLib/UefiRuntimeServicesTableLib.inf

[Components]
  KernReserveLowPkg/KernReserveLow/KernReserveLow.inf

[BuildOptions]
  # RELEASE drops the debug log/NVRAM code (KRL_DEBUG follows !MDEPKG_NDEBUG)
  RELEASE_*_*_CC_FLAGS = -DMDEPKG_NDEBUG
