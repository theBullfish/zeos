# Zeos Kernel Build
#
# Produces: BOOTZ.EFI — the Zeos UEFI bootloader
# Test:     make run (launches QEMU with OVMF)
#

CC       = gcc
LD       = ld
OBJCOPY  = objcopy

# GNU-EFI paths
EFI_INC  = /usr/include/efi
EFI_INC2 = /usr/include/efi/x86_64
EFI_LIB  = /usr/lib
EFI_CRT  = /usr/lib/crt0-efi-x86_64.o
EFI_LDS  = /usr/lib/elf_x86_64_efi.lds

# Compiler flags — freestanding, position-independent, UEFI ABI
CFLAGS   = -ffreestanding \
           -fno-stack-protector \
           -fno-stack-check \
           -fpic \
           -fshort-wchar \
           -mno-red-zone \
           -maccumulate-outgoing-args \
           -Wall -Wextra \
           -I$(EFI_INC) \
           -I$(EFI_INC2) \
           -Iboot \
           -DGNU_EFI_USE_MS_ABI \
           -DHAVE_USE_MS_ABI

# Linker flags
LDFLAGS  = -nostdlib \
           -znocombreloc \
           -T $(EFI_LDS) \
           -shared \
           -Bsymbolic \
           -L$(EFI_LIB) \
           $(EFI_CRT)

LIBS     = -lefi -lgnuefi

# Source files
SRCS     = boot/main.c boot/fb.c boot/idt.c boot/keyboard.c boot/pci.c boot/pmm.c boot/vmm.c boot/heap.c boot/timer.c boot/signal.c boot/shell.c
OBJS     = $(SRCS:.c=.o)

# Output
TARGET   = BOOTZ.EFI

# Build directory for QEMU disk image
BUILDDIR = build
ESP      = $(BUILDDIR)/esp

.PHONY: all clean run

all: $(BUILDDIR)/$(TARGET)

# Compile C to object files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Link to shared object
$(BUILDDIR)/zeos.so: $(OBJS)
	@mkdir -p $(BUILDDIR)
	$(LD) $(LDFLAGS) $(OBJS) -o $@ $(LIBS)

# Convert to PE/COFF EFI executable
$(BUILDDIR)/$(TARGET): $(BUILDDIR)/zeos.so
	$(OBJCOPY) -j .text -j .sdata -j .data -j .dynamic \
		-j .dynsym -j .rel -j .rela -j .reloc \
		--target=efi-app-x86_64 $< $@
	@echo ""
	@echo "  Built: $@"
	@echo "  Zeos UEFI bootloader ready."
	@echo ""

# Create a FAT32 ESP image and run in QEMU
run: $(BUILDDIR)/$(TARGET)
	@mkdir -p $(ESP)/EFI/BOOT
	cp $(BUILDDIR)/$(TARGET) $(ESP)/EFI/BOOT/BOOTX64.EFI
	qemu-system-x86_64 \
		-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
		-drive if=pflash,format=raw,file=$(BUILDDIR)/OVMF_VARS.fd \
		-drive format=raw,file=fat:rw:$(ESP) \
		-m 256M \
		-net none \
		-serial stdio \
		-display gtk \
		2>&1

# Copy OVMF vars (writable copy needed per QEMU instance)
$(BUILDDIR)/OVMF_VARS.fd:
	@mkdir -p $(BUILDDIR)
	cp /usr/share/OVMF/OVMF_VARS_4M.fd $@

# Ensure OVMF vars exist before run
run: $(BUILDDIR)/OVMF_VARS.fd

clean:
	rm -rf $(OBJS) $(BUILDDIR)
