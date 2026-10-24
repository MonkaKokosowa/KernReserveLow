# KernReserveLow

A small UEFI driver for OpenCore that stops macOS from using the lowest ~159 MB of RAM (about 155 MB of it is actual usable RAM). It fixed USB drives
randomly disconnecting on my AMD laptop (Lenovo LOQ 15ARP9, Ryzen 7 7435HS) running macOS 15.

## the symptom

USB storage worked fine for a while and then dropped out. The drive vanished, macOS complained it wasn't ejected
properly, and a moment later it reconnected. That's annoying with a flash drive. With macOS itself on a USB SSD it
means apps refuse to open (`-10810`) and the whole system eventually freezes.

It always happened at the same point: after reading roughly 5-8 GB without a break, or when apps filled up the RAM
and macOS started swapping.

In the log it looks like this:

```
AppleUSBXHCICommandRing::stopEndpoint: hardware is unavailable
IOUSBMassStorageDriver: ... usb reset ...
XHC3@61000000: AppleUSBHostController::hardwareExceptionThreadCallGated: 0x00000020 (system error)
XHC3@61000000: AppleUSBHostController::hardwareExceptionThreadCallGated: attempting recovery
```

`0x00000020` is the controller's Host System Error bit. The controller tried to read or write RAM and the access
failed.

## the cause (as far as i could measure it)

I wrote a debug kext that kept reading the USB controller's transfer rings and recorded where in physical memory
the data buffers were. Every time the error hit, some of the buffers were in very low RAM:

| run | buffers in flight when it broke |
|---|---|
| 1 | `0x138000` |
| 2 | `0x12c0000` to `0x52df000` (18-83 MB) |
| 3 | `0x82bc000` to `0x95ef000` (130-150 MB) |
| 4 | `0x78000` to `0x7b000` |

Buffers from about 255 MB upwards never caused a problem. So on this laptop, the AMD XHCI controllers just can't
DMA into RAM below roughly `0x9F14000`. I don't know why. The firmware lists that range as normal free memory and
the IOMMU is off.

The reason it only shows up after a while: XNU keeps the RAM below the kernel image for last and only starts
handing it out when free memory gets low. So everything works until memory gets tight, and then the next big USB
transfer lands down there and kills the controller.

## what the driver does

It wraps `gBS->GetMemoryMap`. In every memory map it returns, free descriptors (`EfiConventionalMemory`,
`EfiBootServicesCode`, `EfiBootServicesData`) that sit completely inside `0x0` to `0x9F14000` get their type
changed to `EfiReservedMemoryType`. XNU never puts reserved ranges into its page pool, so that RAM can't end up as a
USB buffer anymore.

The important part is that nothing actually gets allocated in the firmware. boot.efi can still put its own stuff
there while it loads the kernel. I tried OpenCore's `UEFI -> ReservedMemory` first, which does allocate the RAM,
and boot.efi just stopped with `EB|STOP 0x16` and a forbidden sign.

I also tried the `EFI_MEMORY_KERN_RESERVED` attribute that XNU has for "pages which can't be used for I/O". It
only works for a handful of pages. With a range this big, XNU hands the leftover pages to the normal memory pool
anyway.

It costs about 155 MB of RAM, which I'm fine with.

Starting at 0 is on purpose, because the first MB failed too. XNU uses its first three pages (low globals and the
real-mode trampoline) by fixed address, without looking at the memory map, so that's safe.

Since 1.1 (v4) it only does that while macOS is starting. 1.0 changed the map for every OS OpenCore started, and
Windows' boot loader just sits at the OEM logo when the whole low RAM is "reserved". So the driver also wraps
`gBS->StartImage` and only changes the map while an Apple boot loader runs: a file called `boot.efi` or
`tmbootpicker.efi`, or anything under `com.apple.recovery.boot`. That's the same check OpenCore itself uses
(`OcGetBootDevicePathType`). OpenCore loads `UEFI -> Drivers` after it installed its own `StartImage` hooks, so this
wrapper sits on the outside and sees every image start. Keep `LoadEarly` off.

If you want the old behaviour back for some reason, put `always` in the driver's `Arguments` in config.plist.

## RELEASE and DEBUG

There are two builds, kind of like OpenCore itself:

- **RELEASE** just does the memory map thing. No log file, no NVRAM writes, nothing on screen. Use this one normally.
- **DEBUG** does exactly the same, plus it keeps a small log (see "checking that it works" below). Use it to make sure
  the driver actually does its thing on your machine, then switch to RELEASE.

## installing

1. Put `KernReserveLow.efi` (from `release/KernReserveLow-1.0.0-RELEASE.zip`, or the DEBUG one) into
   `EFI/OC/Drivers`.
2. Add it to `UEFI -> Drivers`:

   | key | value |
   |---|---|
   | Path | `KernReserveLow.efi` |
   | Enabled | `true` |
   | LoadEarly | `false` |
   | Arguments | empty |

3. Don't use `ReservedMemory` for that range at the same time.

`RANGE_HI` is `0x9F14000` because on my laptop that's where the free RAM below the lowest possible kernel
position ends (the lowest valid KASLR slide is 80, which is `0xA100000`). If you use this on another machine,
check your memory map first. OpenCore's `OpenShell` has `memmap`.

## checking that it works

With the **DEBUG** build, every boot the driver appends what it did on the previous boot to `\KernReserveLow.log`
on the OpenCore partition. It saves that in an NVRAM variable (`KernReserveLow`, GUID
`5A1B0C3D-7E2F-4A61-9B8C-4D5E6F708192`) in between. On my laptop the log looks like this:

```
=== KernReserveLow v3 (retype to Reserved, from 0) load 2026-10-11 16:34:35, range 0x0-0x9F14000
previous boot: 3 GetMemoryMap calls, last call retyped 3 descriptors / 39839 pages (155 MB), 0 not listed
  0x0 - 0x9F000
  0x100000 - 0x9B00000
  0x9E00000 - 0x9F00000
GetMemoryMap hook installed, ExitBootServices event: Success
```

The RELEASE build doesn't write any of that. With either build, this should stay empty in macOS:

```sh
log show --last boot --predicate 'eventMessage CONTAINS "hardwareException"'
```

My test was reading a 15 GB file from a USB stick while the file cache filled the RAM all the way up, plus a
script that filled memory until macOS swapped about 1.6 GB to the USB system SSD. Both died every single time
before. With this driver both go through fine.

## building

It's an EDK2 driver now, built the same way as acidanthera's stuff (their `ocbuild` `efibuild.sh`, pinned in
`tools/`). Works on macOS and Linux:

```sh
./build_krl.tool     # -> Binaries/DEBUG/KernReserveLow.efi and Binaries/RELEASE/KernReserveLow.efi
```

- macOS: Xcode command line tools, plus `nasm`, `iasl` and `mtoc` on your PATH. If they're missing, efibuild.sh offers
  to drop acidanthera's prebuilt ones into `/usr/local/bin` (with sudo). I keep them in a folder of my own instead.
- Linux: gcc or clang, `nasm`, `iasl` (`acpica-tools`), `uuid-dev`, python3. Output folders get the toolchain name
  in front (`GCC-RELEASE`, ...).

The first run clones acidanthera's EDK2 (`audk`) into `UDK/` and builds its BaseTools, which takes a few minutes.
RELEASE is built with `MDEPKG_NDEBUG`, which drops all the logging code.

The old gnu-efi version (1.0) is in `legacy-gnu-efi/` with its Makefile, in case anyone wants it.

## files

- `KernReserveLow/KernReserveLow.c` + `.inf`: the driver
- `KernReserveLowPkg.dsc` / `.dec`, `build_krl.tool`, `tools/efibuild.sh`: the EDK2 build
- `legacy-gnu-efi/`: 1.0, gnu-efi + Makefile
- `release/`: the RELEASE and DEBUG builds, plus `SHA256SUMS`
