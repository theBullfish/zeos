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
           -I../installers/x86_64 \
           -Ilib \
           -Ilib/mbedtls/include \
           -MMD -MP \
           -DGNU_EFI_USE_MS_ABI \
           -DHAVE_USE_MS_ABI \
           -DZEOS_VERSION='"alpha-0.2"' \
           -DZEOS_COMMIT='"$(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)"' \
           $(EXTRA_CFLAGS)

# Linker flags
LDFLAGS  = -nostdlib \
           -znocombreloc \
           -T $(EFI_LDS) \
           -shared \
           -Bsymbolic \
           -L$(EFI_LIB) \
           $(EFI_CRT)

LIBS     = -lefi -lgnuefi
# libgcc: __udivti3 / __umodti3 (128-bit div/mod) used by QuickJS BigInt.
LIBGCC   = $(shell $(CC) -print-libgcc-file-name)

# Source files
SRCS     = lib/lodepng/lodepng.c \
           boot/icon_svg.c \
           boot/main.c boot/fb.c boot/serial.c boot/kprint.c boot/splash.c \
           boot/panic.c ../installers/x86_64/gdt.c ../installers/x86_64/idt.c ../installers/x86_64/hal_x86.c \
           ../installers/x86_64/acpi.c ../installers/x86_64/aml.c ../installers/x86_64/lapic.c ../installers/x86_64/ioapic.c \
           boot/keyboard.c boot/pci.c boot/hwdb.c boot/pmm.c ../installers/x86_64/vmm.c boot/heap.c \
           boot/timer.c boot/timeofday.c boot/signal.c boot/zplus.c boot/zplus_zir.c boot/zp_runtime.c \
           boot/std_btree.c boot/std_json.c boot/std_regex.c boot/std_http.c boot/std_tls_server.c \
           boot/sigviz.c boot/vault.c \
           boot/net.c boot/net_chain.c boot/net_virtio.c boot/net_e1000.c boot/net_rtl8169.c boot/net_rtl8139.c boot/net_arp.c boot/net_ip.c \
           boot/net_udp.c boot/net_tcp.c boot/net_tcp6.c boot/net_dns.c boot/net_http.c boot/net_ntp.c boot/net_ws.c \
           boot/net_ipv6.c boot/net_dhcpv6.c \
           boot/shell.c boot/anim.c boot/ui.c boot/cursor.c boot/net_dhcp.c boot/net_tls.c boot/browser.c \
           boot/mbedtls_platform.c boot/font.c boot/wm.c boot/compositor.c \
           boot/b3.c boot/hw_discover.c boot/chain.c boot/mouse.c boot/firstboot.c \
           boot/mde.c boot/keybinds.c boot/palette.c boot/inspector.c \
           boot/dock.c boot/access.c boot/desktop.c boot/workspaces.c \
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
           ../installers/x86_64/msix.c \
           boot/usb_msc.c \
           boot/usb_hid.c \
           boot/usb_uvc.c \
           boot/bt_usb.c \
           boot/bt_hci.c \
           boot/bt_l2cap.c \
           boot/usb_cdc.c \
           boot/usb_eth.c \
           boot/net_rtl8188eu.c \
           boot/wpa.c \
           boot/block.c \
           boot/block_chain.c \
           boot/mde_chain.c \
           boot/gpt.c \
           boot/fat32.c \
           boot/installer.c \
           boot/hotcorners.c \
           boot/notify.c \
           boot/firewall.c \
           boot/crypto_disk.c \
           boot/print.c \
           boot/calendar.c \
           boot/theme_runtime.c \
           boot/icon_render.c \
           boot/updater.c \
           boot/gpu_virtio.c \
           boot/gpu_nvidia.c \
           boot/gpu_compute.c \
           boot/gpu_goya.c \
           boot/dom_sub.c \
           boot/scheduler.c \
           ../installers/x86_64/smp.c \
           boot/persistence.c \
           boot/hotplug.c \
           ../installers/x86_64/suspend.c \
           boot/battery.c \
           boot/ec.c \
           boot/brightness.c \
           boot/idle.c \
           boot/lockscreen.c \
           boot/identity.c \
           boot/power_buttons.c \
           boot/settings_registry.c \
           boot/ui_undo.c \
           boot/ui_context_menu.c \
           boot/ui_hover.c \
           boot/ui_dirty.c \
           boot/quick_look.c \
           boot/image_viewer.c \
           boot/jpeg.c \
           boot/md_render.c \
           boot/chat_e2ee.c \
           boot/build_runner.c \
           boot/calculator.c \
           boot/editor.c \
           boot/ui_states.c \
           boot/fs_event.c \
           boot/file_mgr.c \
           boot/notes_zeos.c \
           boot/chat_zeos.c \
           boot/kv_polish.c \
           boot/web_polish.c \
           boot/build_polish.c \
           boot/notes_polish.c \
           boot/chat_polish.c \
           boot/wm_persist.c \
           boot/activity.c \
           boot/welcome.c \
           boot/tests.c

# mbedTLS library sources (3.6 LTS, vendored at lib/mbedtls/)
MBEDTLS_SRCS = $(wildcard lib/mbedtls/library/*.c)

# QuickJS (ES2020 engine) + qjs_port.c glue, and vendored musl-libm (lib/zeosm).
# I.7: JavaScript engine for the browser. Third-party C; compiled -w. zeosm's
# shim headers (math.h/stdlib.h/…) are scoped to lib/zeosm ONLY so they never
# shadow the system headers the rest of the kernel uses.
QUICKJS_SRCS = $(wildcard lib/quickjs/*.c)
ZEOSM_SRCS   = $(wildcard lib/zeosm/*.c)
QUICKJS_OBJS = $(QUICKJS_SRCS:.c=.o)
ZEOSM_OBJS   = $(ZEOSM_SRCS:.c=.o)

OBJS     = $(SRCS:.c=.o) $(MBEDTLS_SRCS:.c=.o) $(QUICKJS_OBJS) $(ZEOSM_OBJS)

# ── Embedded fonts (TTF -> ELF via objcopy) ──
FONT_INTER  = ../assets/fonts/inter/Inter-Regular.ttf
FONT_JBMONO = ../assets/fonts/jetbrains-mono/JetBrainsMono-Regular.ttf
FONT_NOTO   = ../assets/fonts/noto/NotoSans-Fallback.ttf
FONT_OBJS   = boot/font_inter.o boot/font_jbmono.o boot/font_noto.o

# ── Embedded firmware blobs (binary -> ELF via objcopy) ──
FW_RTL8188EU       = lib/firmware/rtl8188eufw.bin
FW_GOYA_BOOT_FIT   = lib/firmware/habanalabs/goya/goya-boot-fit.itb
FW_GOYA_LINUX_FIT  = lib/firmware/habanalabs/goya/goya-fit.itb

# NVIDIA GSP firmware (Turing TU102 family, Ampere GA10x family).
# Sourced from /lib/firmware/nvidia/<chip>/gsp/ (linux-firmware 535.113.01),
# zstd-decompressed at build time. Each chip family has 4 blobs:
#   bootloader.bin, booter_load.bin, booter_unload.bin, gsp.bin
# objcopy emits _binary_<file>_bin_{start,end,size} per blob.
FW_NV_TU102_DIR     = lib/firmware/nvidia/tu102/gsp
FW_NV_GA102_DIR     = lib/firmware/nvidia/ga102/gsp
FW_NV_TU102_OBJS    = $(FW_NV_TU102_DIR)/tu102_bootloader.o \
                      $(FW_NV_TU102_DIR)/tu102_booter_load.o \
                      $(FW_NV_TU102_DIR)/tu102_booter_unload.o \
                      $(FW_NV_TU102_DIR)/tu102_gsp.o
FW_NV_GA102_OBJS    = $(FW_NV_GA102_DIR)/ga102_bootloader.o \
                      $(FW_NV_GA102_DIR)/ga102_booter_load.o \
                      $(FW_NV_GA102_DIR)/ga102_booter_unload.o \
                      $(FW_NV_GA102_DIR)/ga102_gsp.o

FW_OBJS      = lib/firmware/rtl8188eufw.o \
               lib/firmware/habanalabs/goya/goya-boot-fit.o \
               lib/firmware/habanalabs/goya/goya-fit.o \
               $(FW_NV_TU102_OBJS) \
               $(FW_NV_GA102_OBJS)

# AP bring-up trampoline (assembled flat binary embedded as ELF object).
SMP_OBJS    = ../installers/x86_64/smp_trampoline.o

# ── D.9: desktop icons (SVG -> 48x48 RGBA PNG at build time -> ELF via objcopy) ──
# rsvg-convert rasterizes; objcopy embeds; icon_svg.c decodes (lodepng) + tints.
ICON_SVG_DIR = ../assets/icons
ICON_OBJS    = boot/icon_folder.o boot/icon_code.o boot/icon_gear.o
WALLPAPER_OBJS = boot/wallpaper.o

# D.9 icons: rasterize the SVG to 48x48 RGBA PNG, then objcopy-embed it with a
# clean symbol name (_binary_icon_<name>_png_{start,end,size}).
boot/icon_folder.o: $(ICON_SVG_DIR)/ui-basic/Folder.svg
	@mkdir -p build/icons
	rsvg-convert -w 48 -h 48 $< -o build/icons/icon_folder.png
	$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
		--redefine-sym _binary_build_icons_icon_folder_png_start=_binary_icon_folder_png_start \
		--redefine-sym _binary_build_icons_icon_folder_png_end=_binary_icon_folder_png_end \
		--redefine-sym _binary_build_icons_icon_folder_png_size=_binary_icon_folder_png_size \
		build/icons/icon_folder.png $@

boot/icon_code.o: $(ICON_SVG_DIR)/ui-basic/Code.svg
	@mkdir -p build/icons
	rsvg-convert -w 48 -h 48 $< -o build/icons/icon_code.png
	$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
		--redefine-sym _binary_build_icons_icon_code_png_start=_binary_icon_code_png_start \
		--redefine-sym _binary_build_icons_icon_code_png_end=_binary_icon_code_png_end \
		--redefine-sym _binary_build_icons_icon_code_png_size=_binary_icon_code_png_size \
		build/icons/icon_code.png $@

boot/icon_gear.o: $(ICON_SVG_DIR)/ui-basic/Gear.svg
	@mkdir -p build/icons
	rsvg-convert -w 48 -h 48 $< -o build/icons/icon_gear.png
	$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
		--redefine-sym _binary_build_icons_icon_gear_png_start=_binary_icon_gear_png_start \
		--redefine-sym _binary_build_icons_icon_gear_png_end=_binary_icon_gear_png_end \
		--redefine-sym _binary_build_icons_icon_gear_png_size=_binary_icon_gear_png_size \
		build/icons/icon_gear.png $@

# ── D.10: default desktop wallpaper (SVG -> 480x270 RGBA PNG -> ELF) ──
# rsvg rasterizes a smooth gradient (compresses < 48KB so it fits a VAULT file).
# Seeded into VAULT at boot; desktop.c loads it back via vault_read + lodepng and
# nearest-neighbour scale-blits x4 to fill 1920x1080.
boot/wallpaper.o: assets/wallpaper.svg
	@mkdir -p build
	rsvg-convert -w 480 -h 270 $< -o build/wallpaper.png
	$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
		--redefine-sym _binary_build_wallpaper_png_start=_binary_wallpaper_png_start \
		--redefine-sym _binary_build_wallpaper_png_end=_binary_wallpaper_png_end \
		--redefine-sym _binary_build_wallpaper_png_size=_binary_wallpaper_png_size \
		build/wallpaper.png $@

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

# F.4: Noto Sans fallback tier
boot/font_noto.o: $(FONT_NOTO)
	$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
		--redefine-sym _binary____assets_fonts_noto_NotoSans_Fallback_ttf_start=_binary_noto_regular_ttf_start \
		--redefine-sym _binary____assets_fonts_noto_NotoSans_Fallback_ttf_end=_binary_noto_regular_ttf_end \
		--redefine-sym _binary____assets_fonts_noto_NotoSans_Fallback_ttf_size=_binary_noto_regular_ttf_size \
		$< $@

# Embed RTL8188EU firmware blob. cd into the firmware dir so objcopy
# emits clean symbol names (_binary_rtl8188eufw_bin_{start,end,size}).
lib/firmware/rtl8188eufw.o: $(FW_RTL8188EU)
	cd lib/firmware && $(OBJCOPY) --input-target=binary \
		--output-target=elf64-x86-64 \
		--binary-architecture=i386:x86-64 \
		rtl8188eufw.bin rtl8188eufw.o

# Embed Habana Goya FIT firmware blobs (preboot + Linux). cd into the
# firmware directory so objcopy emits clean symbol names of the form
# _binary_goya_boot_fit_itb_{start,end,size} and
# _binary_goya_fit_itb_{start,end,size}.
# If the blob source file is missing, build an empty .o so the
# weak-linked symbols collapse to start==end and gpu_goya.c reports
# "no firmware" honestly.
lib/firmware/habanalabs/goya/goya-boot-fit.o: $(FW_GOYA_BOOT_FIT)
	@mkdir -p lib/firmware/habanalabs/goya
	@if [ -f $(FW_GOYA_BOOT_FIT) ]; then \
		cd lib/firmware/habanalabs/goya && $(OBJCOPY) \
			--input-target=binary \
			--output-target=elf64-x86-64 \
			--binary-architecture=i386:x86-64 \
			goya-boot-fit.itb goya-boot-fit.o ; \
	else \
		echo "[fw] $(FW_GOYA_BOOT_FIT) absent -- emitting empty stub"; \
		printf '' > lib/firmware/habanalabs/goya/_empty.bin ; \
		cd lib/firmware/habanalabs/goya && $(OBJCOPY) \
			--input-target=binary \
			--output-target=elf64-x86-64 \
			--binary-architecture=i386:x86-64 \
			--redefine-sym _binary__empty_bin_start=_binary_goya_boot_fit_itb_start \
			--redefine-sym _binary__empty_bin_end=_binary_goya_boot_fit_itb_end \
			--redefine-sym _binary__empty_bin_size=_binary_goya_boot_fit_itb_size \
			_empty.bin goya-boot-fit.o ; \
	fi

lib/firmware/habanalabs/goya/goya-fit.o: $(FW_GOYA_LINUX_FIT)
	@mkdir -p lib/firmware/habanalabs/goya
	@if [ -f $(FW_GOYA_LINUX_FIT) ]; then \
		cd lib/firmware/habanalabs/goya && $(OBJCOPY) \
			--input-target=binary \
			--output-target=elf64-x86-64 \
			--binary-architecture=i386:x86-64 \
			goya-fit.itb goya-fit.o ; \
	else \
		echo "[fw] $(FW_GOYA_LINUX_FIT) absent -- emitting empty stub"; \
		printf '' > lib/firmware/habanalabs/goya/_empty2.bin ; \
		cd lib/firmware/habanalabs/goya && $(OBJCOPY) \
			--input-target=binary \
			--output-target=elf64-x86-64 \
			--binary-architecture=i386:x86-64 \
			--redefine-sym _binary__empty2_bin_start=_binary_goya_fit_itb_start \
			--redefine-sym _binary__empty2_bin_end=_binary_goya_fit_itb_end \
			--redefine-sym _binary__empty2_bin_size=_binary_goya_fit_itb_size \
			_empty2.bin goya-fit.o ; \
	fi

# NVIDIA GSP firmware blobs. Each blob is renamed with a chip prefix
# before objcopy so the emitted _binary_<name>_bin_{start,end,size}
# symbols are unique across chip families. Missing blobs collapse to
# an empty stub (start==end) so gpu_nvidia.c can report "absent"
# honestly without breaking the link, mirroring the Goya pattern.
# If a local blob is missing, try to source it from the host's
# linux-firmware install (zstd-decompressed). This keeps the kernel
# repo free of NVIDIA-signed binaries while letting any developer
# with linux-firmware-nvidia installed build a fully-armed Stage 2.
define NV_FW_RULE
$(1)/$(3).bin:
	@mkdir -p $(1)
	@if [ ! -s $(1)/$(3).bin ] && [ -s /lib/firmware/nvidia/$(2)/gsp/$(3)-535.113.01.bin.zst ]; then \
		echo "[fw] sourcing nvidia/$(2)/gsp/$(3).bin from host linux-firmware"; \
		zstd -dc /lib/firmware/nvidia/$(2)/gsp/$(3)-535.113.01.bin.zst > $(1)/$(3).bin 2>/dev/null || rm -f $(1)/$(3).bin; \
	fi
	@touch $(1)/$(3).bin

$(1)/$(2)_$(3).o: $(1)/$(3).bin
	@mkdir -p $(1)
	@if [ -s $(1)/$(3).bin ]; then \
		cp $(1)/$(3).bin $(1)/$(2)_$(3).bin ; \
		cd $(1) && $$(OBJCOPY) \
			--input-target=binary \
			--output-target=elf64-x86-64 \
			--binary-architecture=i386:x86-64 \
			$(2)_$(3).bin $(2)_$(3).o ; \
	else \
		echo "[fw] nvidia/$(2)/gsp/$(3).bin absent -- emitting empty stub"; \
		printf '\0' > $(1)/_empty_$(2)_$(3).bin ; \
		cd $(1) && $$(OBJCOPY) \
			--input-target=binary \
			--output-target=elf64-x86-64 \
			--binary-architecture=i386:x86-64 \
			--redefine-sym _binary__empty_$(2)_$(3)_bin_start=_binary_$(2)_$(3)_bin_start \
			--redefine-sym _binary__empty_$(2)_$(3)_bin_end=_binary_$(2)_$(3)_bin_end \
			--redefine-sym _binary__empty_$(2)_$(3)_bin_size=_binary_$(2)_$(3)_bin_size \
			_empty_$(2)_$(3).bin $(2)_$(3).o ; \
	fi
endef

$(eval $(call NV_FW_RULE,$(FW_NV_TU102_DIR),tu102,bootloader))
$(eval $(call NV_FW_RULE,$(FW_NV_TU102_DIR),tu102,booter_load))
$(eval $(call NV_FW_RULE,$(FW_NV_TU102_DIR),tu102,booter_unload))
$(eval $(call NV_FW_RULE,$(FW_NV_TU102_DIR),tu102,gsp))
$(eval $(call NV_FW_RULE,$(FW_NV_GA102_DIR),ga102,bootloader))
$(eval $(call NV_FW_RULE,$(FW_NV_GA102_DIR),ga102,booter_load))
$(eval $(call NV_FW_RULE,$(FW_NV_GA102_DIR),ga102,booter_unload))
$(eval $(call NV_FW_RULE,$(FW_NV_GA102_DIR),ga102,gsp))

# AP bring-up trampoline. NASM assembles the 16->32->64 trampoline
# to a flat 4 KiB binary; objcopy embeds it as an ELF section so the
# kernel can copy it verbatim to physical 0x8000 before SIPI.
../installers/x86_64/smp_trampoline.bin: ../installers/x86_64/smp_trampoline.asm
	nasm -f bin -o $@ $<

../installers/x86_64/smp_trampoline.o: ../installers/x86_64/smp_trampoline.bin
	cd ../installers/x86_64 && $(OBJCOPY) --input-target=binary \
		--output-target=elf64-x86-64 \
		--binary-architecture=i386:x86-64 \
		smp_trampoline.bin smp_trampoline.o

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

# QuickJS: system headers for prototypes, local headers for cutils.h/quickjs.h,
# links against zeosm's libm. -w silences third-party warnings.
lib/quickjs/%.o: lib/quickjs/%.c
	$(CC) $(CFLAGS) -Ilib/quickjs -DCONFIG_VERSION='"zeos-0.2"' -w -c $< -o $@

# musl-libm: -Ilib/zeosm scoped HERE so its shim math.h/stdlib.h/limits.h don't
# leak into the rest of the kernel build.
lib/zeosm/%.o: lib/zeosm/%.c
	$(CC) $(CFLAGS) -Ilib/zeosm -w -c $< -o $@

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

# Link to shared object (includes embedded font and firmware blobs)
$(BUILDDIR)/zeos.so: $(OBJS) $(FONT_OBJS) $(FW_OBJS) $(SMP_OBJS) $(ICON_OBJS) $(WALLPAPER_OBJS)
	@mkdir -p $(BUILDDIR)
	$(LD) $(LDFLAGS) $(OBJS) $(FONT_OBJS) $(FW_OBJS) $(SMP_OBJS) $(ICON_OBJS) $(WALLPAPER_OBJS) -o $@ $(LIBS) $(LIBGCC)

# Convert to PE/COFF EFI executable
$(BUILDDIR)/$(TARGET): $(BUILDDIR)/zeos.so
	$(OBJCOPY) -j .text -j .sdata -j .data -j .dynamic \
		-j .dynsym -j .rel -j .rela -j .reloc \
		--target=efi-app-x86_64 $< $@
	@echo ""
	@echo "  Built: $@"
	@echo "  Zeos UEFI bootloader ready."
	@echo ""

# Persistent VAULT backing image: NVMe drive that survives QEMU
# `system_reset` so the VAULT journal + chain registry snapshot
# round-trip across reboots. 8 MB is enough for the 2 MB ramdisk
# image plus headroom for future growth.
VAULT_IMG  = $(BUILDDIR)/vault.img
VAULT_SIZE = 8

$(VAULT_IMG):
	@mkdir -p $(BUILDDIR)
	dd if=/dev/zero of=$(VAULT_IMG) bs=1M count=$(VAULT_SIZE) 2>/dev/null

# Create a FAT32 ESP image and run in QEMU
run: $(BUILDDIR)/$(TARGET) $(VAULT_IMG)
	@mkdir -p $(ESP)/EFI/BOOT
	cp $(BUILDDIR)/$(TARGET) $(ESP)/EFI/BOOT/BOOTX64.EFI
	qemu-system-x86_64 \
		-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
		-drive if=pflash,format=raw,file=$(BUILDDIR)/OVMF_VARS.fd \
		-drive format=raw,file=fat:rw:$(ESP) \
		-drive if=none,id=zeosvault,file=$(VAULT_IMG),format=raw \
		-device nvme,drive=zeosvault,serial=ZEOSVAULT \
		-m 256M \
		-device virtio-net-pci,netdev=n0 \
		-netdev user,id=n0 \
		-serial stdio \
		-display gtk \
		2>&1

# Two-GPU virtio-vga test boot. Validates the multi-GPU /
# multi-scanout path in gpu_virtio.c — each `-device virtio-vga`
# enumerates as its own CHAIN_GPU_<n> with its own resource-id
# namespace and its own per-display backing memory. selftest's
# GPU line should report:
#     GPU ........... PASS (2 device(s), 2 display(s))
# (QEMU's virtio-vga exposes one scanout per device; see
# docs/GPU_HOLES.md and the top of kernel/boot/gpu_virtio.c.)
run-multigpu: $(BUILDDIR)/$(TARGET) $(VAULT_IMG) $(BUILDDIR)/OVMF_VARS.fd
	@mkdir -p $(ESP)/EFI/BOOT
	cp $(BUILDDIR)/$(TARGET) $(ESP)/EFI/BOOT/BOOTX64.EFI
	qemu-system-x86_64 \
		-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
		-drive if=pflash,format=raw,file=$(BUILDDIR)/OVMF_VARS.fd \
		-drive format=raw,file=fat:rw:$(ESP) \
		-drive if=none,id=zeosvault,file=$(VAULT_IMG),format=raw \
		-device nvme,drive=zeosvault,serial=ZEOSVAULT \
		-m 512M \
		-device virtio-vga \
		-device virtio-vga \
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

# Headless multi-core run for SMP validation. Serial-only (no GTK) so
# we sidestep the snap-shell GTK_PATH issue that pulls a stale snap
# libpthread into qemu when -display gtk loads its GTK module. -smp 4
# enables four cores; the per-CPU preempt slot landed in commit b6fd147
# unblocks lifted CHAIN_NET_TX/BLOCK/AUDIO from BSP affinity.
run-headless: $(BUILDDIR)/$(TARGET) $(VAULT_IMG) $(BUILDDIR)/OVMF_VARS.fd
	@mkdir -p $(ESP)/EFI/BOOT
	cp $(BUILDDIR)/$(TARGET) $(ESP)/EFI/BOOT/BOOTX64.EFI
	env -i HOME=$$HOME PATH=/usr/bin:/bin \
	qemu-system-x86_64 \
		-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
		-drive if=pflash,format=raw,file=$(BUILDDIR)/OVMF_VARS.fd \
		-drive format=raw,file=fat:rw:$(ESP) \
		-drive if=none,id=zeosvault,file=$(VAULT_IMG),format=raw \
		-device nvme,drive=zeosvault,serial=ZEOSVAULT \
		-m 512M -smp 4 \
		-device virtio-net-pci,netdev=n0 \
		-netdev user,id=n0 \
		-serial stdio \
		-display none \
		-monitor none \
		2>&1
run-headless: $(BUILDDIR)/OVMF_VARS.fd

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
	rm -rf $(OBJS) $(OBJS:.o=.d) $(FONT_OBJS) $(FW_OBJS) $(SMP_OBJS) \
	       ../installers/x86_64/smp_trampoline.bin $(BUILDDIR)
