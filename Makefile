# builds KernReserveLow.efi with gnu-efi (debian/ubuntu: sudo apt install gnu-efi)
#   make            -> build/RELEASE/KernReserveLow.efi and build/DEBUG/KernReserveLow.efi
#   make release    -> only the RELEASE one
#   make debug      -> only the DEBUG one (writes \KernReserveLow.log, see the README)
#   make OUT=dir    -> puts the build folders into dir instead of build

OUT     ?= build
EFIINC  ?= /usr/include/efi
LIBDIR  ?= /usr/lib
CRT0    ?= $(LIBDIR)/crt0-efi-x86_64.o
LDS     ?= $(LIBDIR)/elf_x86_64_efi.lds

CFLAGS  = -I$(EFIINC) -I$(EFIINC)/x86_64 -I$(EFIINC)/protocol \
          -fpic -fshort-wchar -fno-stack-protector -fno-strict-aliasing -ffreestanding \
          -mno-red-zone -maccumulate-outgoing-args -DEFI_FUNCTION_WRAPPER -DGNU_EFI_USE_MS_ABI \
          -Wall -Wextra -Wno-type-limits -O2 -std=gnu11   # type-limits: RANGE_LO is 0 so one check is always false, thats fine
LDFLAGS = -nostdlib -znocombreloc -shared -Bsymbolic -T $(LDS) -L$(LIBDIR) $(CRT0)

all: release debug
release: $(OUT)/RELEASE/KernReserveLow.efi
debug: $(OUT)/DEBUG/KernReserveLow.efi

$(OUT)/RELEASE/KernReserveLow.o: KernReserveLow.c
	mkdir -p $(@D)
	gcc $(CFLAGS) -c $< -o $@

$(OUT)/DEBUG/KernReserveLow.o: KernReserveLow.c
	mkdir -p $(@D)
	gcc $(CFLAGS) -DKRL_DEBUG -c $< -o $@

%/KernReserveLow.so: %/KernReserveLow.o
	ld $(LDFLAGS) $< -o $@ -lefi -lgnuefi

# keep .rodata in the copy list!! gnu-efi 4 puts the strings there and without it the driver loads fine
# but silently does nothing. that one cost me a whole evening
%/KernReserveLow.efi: %/KernReserveLow.so
	objcopy -j .text -j .sdata -j .data -j .rodata -j .dynamic -j .dynsym -j .rel -j .rela -j .rel.* -j .rela.* \
	        -j .reloc -I elf64-x86-64 -O efi-bsdrv-x86_64 $< $@

# only cleans the default build folder, with a literal path (no variables in rm)
clean:
	rm -rf build

.PHONY: all release debug clean
.SECONDARY:
