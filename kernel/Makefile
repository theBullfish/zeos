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
           -Ilib \
           -Ilib/mbedtls/include \
           -MMD -MP \
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
SRCS     = lib/lodepng/lodepng.c \
           boot/main.c boot/fb.c boot/serial.c boot/kprint.c \
           boot/panic.c boot/gdt.c boot/idt.c \
           boot/keyboard.c boot/pci.c boot/pmm.c boot/vmm.c boot/heap.c \
           boot/timer.c boot/signal.c boot/zplus.c boot/sigviz.c boot/vault.c \
           boot/net.c boot/net_chain.c boot/net_virtio.c boot/net_e1000.c boot/net_rtl8169.c boot/net_rtl8139.c boot/net_arp.c boot/net_ip.c \
           boot/net_udp.c boot/net_tcp.c boot/net_dns.c boot/net_http.c \
           boot/shell.c boot/anim.c boot/ui.c boot/cursor.c boot/net_dhcp.c boot/net_tls.c boot/browser.c \
           boot/mbedtls_platform.c boot/font.c boot/wm.c boot/compositor.c \
           boot/b3.c boot/hw_discover.c boot/chain.c boot/mouse.c boot/firstboot.c \
           boot/mde.c boot/keybinds.c boot/palette.c boot/inspector.c \
           boot/dock.c boot/access.c boot/desktop.c \
           boot/panel.c boot/settings.c boot/persona_filter.c boot/persona_anim.c \
           boot/chain_registry.c \
           boot/cfa_handle.c \
           boot/vault_disk.c \
           boot/nvme.c \
           boot/ahci.c \
           boot/hda.c \
           boot/usb_xhci.c \
           boot/usb_ehci.c \
           boot/usb_hc.c \
           boot/usb_hub.c \
           boot/msix.c \
           boot/usb_msc.c \
           boot/usb_hid.c \
           boot/usb_cdc.c \
           boot/usb_eth.c \
           boot/net_rtl8188eu.c \
           boot/block.c \
           boot/block_chain.c \
           boot/mde_chain.c \
           boot/gpt.c \
           boot/fat32.c \
           boot/installer.c \
           boot/hotcorners.c \
           boot/notify.c \
           boot/theme_runtime.c \
           boot/icon_render.c \
           boot/updater.c \
           boot/gpu_virtio.c

# mbedTLS library sources (3.6 LTS, vendored at lib/mbedtls/)
MBEDTLS_SRCS = $(wildcard lib/mbedtls/library/*.c)

OBJS     = $(SRCS:.c=.o) $(MBEDTLS_SRCS:.c=.o)

# ── Embedded fonts (TTF -> ELF via objcopy) ──
FONT_INTER  = ../assets/fonts/inter/Inter-Regular.ttf
FONT_JBMONO = ../assets/fonts/jetbrains-mono/JetBrainsMono-Regular.ttf
FONT_OBJS   = boot/font_inter.o boot/font_jbmono.o

boot/font_inter.o: $(FONT_INTER)
	$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
		--redefine-sym _binary____assets_fonts_inter_Inter_Regular_ttf_start=_binary_inter_regular_ttf_start \
		--redefine-sym _binary____assets_fonts_inter_Inter_Regular_ttf_end=_binary_inter_regular_ttf_end \
		--redefine-sym _binary____assets_fonts_inter_Inter_Regular_ttf_size=_binary_inter_regular_ttf_size \
		$< $@

boot/font_jbmono.o: $(FONT_JBMONO)
	$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
		--redefine-sym _binary____assets_fonts_jetbrains_mono_JetBrainsMono_Regular_ttf_start=_binary_jbmono_regular_ttf_start \
		--redefine-sym _binary____assets_fonts_jetbrains_mono_JetBrainsMono_Regular_ttf_end=_binary_jbmono_regular_ttf_end \
		--redefine-sym _binary____assets_fonts_jetbrains_mono_JetBrainsMono_Regular_ttf_size=_binary_jbmono_regular_ttf_size \
		$< $@

# Output
TARGET   = BOOTZ.EFI

# Build directory for QEMU disk image
BUILDDIR = build
ESP      = $(BUILDDIR)/esp

.PHONY: all clean run usb

all: $(BUILDDIR)/$(TARGET)

# Compile C to object files. mbedtls sources get a quieter flag set so
# upstream warnings don't drown ours; -Wno-array-bounds avoids a known
# false-positive in psa_crypto.c on GCC 13+.
MBEDTLS_CFLAGS = $(CFLAGS) -Wno-unused-parameter -Wno-unused-variable \
                 -Wno-unused-but-set-variable -Wno-sign-compare \
                 -Wno-array-bounds -Wno-stringop-overflow \
                 -Wno-implicit-fallthrough -Wno-maybe-uninitialized \
                 -Wno-cast-function-type

lib/mbedtls/library/%.o: lib/mbedtls/library/%.c
	$(CC) $(MBEDTLS_CFLAGS) -c $< -o $@

# lodepng: PNG decoder. Strip C++ wrappers, file I/O, encoder, and the
# bundled allocators (we route to kmalloc/kfree via shim in browser.c).
LODEPNG_CFLAGS = $(CFLAGS) \
                 -DLODEPNG_NO_COMPILE_CPP \
                 -DLODEPNG_NO_COMPILE_DISK \
                 -DLODEPNG_NO_COMPILE_ENCODER \
                 -DLODEPNG_NO_COMPILE_ALLOCATORS \
                 -Wno-unused-parameter -Wno-unused-variable \
                 -Wno-unused-but-set-variable -Wno-sign-compare \
                 -Wno-implicit-fallthrough -Wno-maybe-uninitialized

lib/lodepng/lodepng.o: lib/lodepng/lodepng.c
	$(CC) $(LODEPNG_CFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Auto-generated header dependencies (-MMD -MP in CFLAGS)
-include $(OBJS:.o=.d)

# Link to shared object (includes embedded font data)
$(BUILDDIR)/zeos.so: $(OBJS) $(FONT_OBJS)
	@mkdir -p $(BUILDDIR)
	$(LD) $(LDFLAGS) $(OBJS) $(FONT_OBJS) -o $@ $(LIBS)

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
		-device virtio-net-pci,netdev=n0 \
		-netdev user,id=n0 \
		-serial stdio \
		-display gtk \
		2>&1

# Copy OVMF vars (writable copy needed per QEMU instance)
$(BUILDDIR)/OVMF_VARS.fd:
	@mkdir -p $(BUILDDIR)
	cp /usr/share/OVMF/OVMF_VARS_4M.fd $@

# Ensure OVMF vars exist before run
run: $(BUILDDIR)/OVMF_VARS.fd

# ── USB boot image (GPT + EFI System Partition) ──────────────────
# Creates a raw disk image that can be dd'd to a USB stick.
# Uses mtools (no root required). Target: ASUS CN60 Chromebox and
# any x86_64 UEFI system.
#
# Flash with: sudo dd if=build/zeos-usb.img of=/dev/sdX bs=4M status=progress
#
USB_IMG  = $(BUILDDIR)/zeos-usb.img
USB_SIZE = 64
USB_OFF  = 1048576

usb: $(BUILDDIR)/$(TARGET)
	@echo ""
	@echo "  Creating $(USB_SIZE)MB USB boot image..."
	dd if=/dev/zero of=$(USB_IMG) bs=1M count=$(USB_SIZE) 2>/dev/null
	sgdisk --clear \
		--new=1:2048:131038 \
		--typecode=1:ef00 \
		--change-name=1:"EFI System" \
		$(USB_IMG) >/dev/null 2>&1
	MTOOLS_SKIP_CHECK=1 mformat -i $(USB_IMG)@@$(USB_OFF) -F -h 32 -t 128 -s 63 ::
	MTOOLS_SKIP_CHECK=1 mmd -i $(USB_IMG)@@$(USB_OFF) ::/EFI
	MTOOLS_SKIP_CHECK=1 mmd -i $(USB_IMG)@@$(USB_OFF) ::/EFI/BOOT
	MTOOLS_SKIP_CHECK=1 mcopy -i $(USB_IMG)@@$(USB_OFF) \
		$(BUILDDIR)/$(TARGET) ::/EFI/BOOT/BOOTX64.EFI
	@echo ""
	@echo "  USB image: $(USB_IMG)"
	@ls -lh $(USB_IMG) | awk '{print "  Size:      " $$5}'
	@echo ""
	@echo "  Flash with:"
	@echo "    sudo dd if=$(USB_IMG) of=/dev/sdX bs=4M status=progress"
	@echo ""

clean:
	rm -rf $(OBJS) $(OBJS:.o=.d) $(FONT_OBJS) $(BUILDDIR)
