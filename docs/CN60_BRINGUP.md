# Z-OS CN60 Bringup Plan

> ASUS CN60 Chromebox × 3 → Z-OS bare-metal targets
> Walk-through done with Brad. This is the plan.

---

## Hardware: ASUS CN60 Chromebox

- **CPU:** Intel Celeron 2955U or Core i3-4010U (Haswell, x86_64)
- **RAM:** 2-4GB DDR3 (upgradeable to 16GB, 2× SO-DIMM)
- **Storage:** 16GB M.2 SSD (replaceable)
- **Display:** HDMI + DisplayPort
- **USB:** 4× USB 3.0
- **Network:** Realtek RTL8168 GbE + Intel WiFi
- **Audio:** 3.5mm out
- **Size:** 124 × 124 × 42mm

---

## Phase 0: Inventory & Prep

### For each CN60, verify:
- [ ] Powers on (12V barrel jack)
- [ ] Connects to HDMI display
- [ ] USB keyboard works
- [ ] Note: which CPU variant (check BIOS or sticker)

### Gather:
- [ ] 3× USB sticks (4GB+ each, will be erased)
- [ ] 3× HDMI cables + monitors (or 1 monitor, rotate)
- [ ] USB keyboard (one is fine, move between boxes)
- [ ] USB-C or USB-A Ethernet adapter (optional, for future networking)
- [ ] Phillips screwdriver (to open case if needed for write-protect screw)

---

## Phase 1: Firmware Unlock

**The CN60 ships with ChromeOS firmware (Coreboot + depthcharge).**
**Stock firmware CANNOT boot standard UEFI applications from USB.**

### Option A: MrChromebox Full UEFI Firmware (RECOMMENDED)

This replaces the ChromeOS firmware entirely with standard UEFI (Coreboot + Tianocore).
Z-OS's `BOOTX64.EFI` will boot natively.

#### Steps:
1. **Remove write-protect screw**
   - Power off, unplug
   - Remove bottom plate (4 rubber feet hide screws)
   - Locate WP screw near the M.2 SSD — it's a small screw with a metallic pad
   - Remove it (or just loosen it — breaking the contact is enough)
   - Reassemble

2. **Enable Developer Mode**
   - Power off
   - Insert paperclip into recovery button hole (bottom of unit)
   - Hold recovery button + press power
   - At recovery screen: press Ctrl+D
   - Confirm with Enter
   - Wait for transition (~5 minutes, it wipes the SSD)
   - On reboot: Ctrl+D to skip the scary screen each time

3. **Flash UEFI firmware**
   - Boot into ChromeOS (sign in as guest is fine)
   - Open terminal: Ctrl+Alt+T → type `shell` → Enter
   - Run MrChromebox script:
     ```bash
     cd; curl -LO mrchromebox.tech/firmware-util.sh && sudo bash firmware-util.sh
     ```
   - Select: **Install/Update UEFI (Full ROM) Firmware**
   - Confirm: YES (this is irreversible without a SPI programmer)
   - Back up stock firmware when prompted (save to USB stick)
   - Wait for flash to complete
   - Power off

4. **Verify**
   - Power on — you should see a Coreboot/Tianocore splash
   - Press Esc or Del to enter UEFI setup
   - Confirm: UEFI boot mode is available

### Option B: Developer Mode USB Boot (LESS RELIABLE)

If you don't want to flash firmware:
1. Enable Developer Mode (step 2 above)
2. At every boot: Ctrl+U to boot from USB
3. **Problem:** ChromeOS depthcharge expects a signed ChromeOS USB image, NOT a standard UEFI executable. This path likely won't work for Z-OS.

**→ Option A is the way. Flash all three.**

---

## Phase 2: Create Boot Media

### On Z13:
```bash
cd /home/z13/zeos/kernel

# Build the kernel
make clean && make

# Build the USB image
make usb

# Flash to USB stick (REPLACE sdX with actual device)
lsblk                    # identify USB stick
sudo dd if=build/zeos-usb.img of=/dev/sdX bs=4M status=progress
sync
```

### Verify the stick:
```bash
sudo fdisk -l /dev/sdX   # should show GPT with EFI System Partition
sudo mount /dev/sdX1 /mnt
ls /mnt/EFI/BOOT/BOOTX64.EFI   # should exist, ~3.6MB
sudo umount /mnt
```

### Make 3 sticks (or reuse 1):
Same image works on all three CN60s — Z-OS self-discovers hardware.

---

## Phase 3: First Boot

### Steps:
1. Insert USB stick into CN60
2. Power on
3. Press Esc/Del/F2 to enter UEFI setup (if MrChromebox firmware)
4. Set USB as first boot device (or use boot menu — usually F10)
5. Save and reboot
6. **Z-OS should boot**

### What you should see:

```
[BOOT] Zeos starting...
[BOOT] Framebuffer: 1920x1080 (or whatever HDMI resolution)
[BOOT] Memory map: XX entries, XX MB available
[BOOT] PMM: XX pages available
[BOOT] VMM: paging enabled
[BOOT] IDT: interrupts configured
[BOOT] PCI: XX devices found
[BOOT] Timer: PIT + TSC calibrated
[BOOT] Signal chain engine ready
[BOOT] VAULT initialized
[BOOT] Z+ interpreter v0.2

zeos> _
```

The shell prompt should be in steel blue (#2E86AB) on dark background (#0D1117).

### If you see nothing:
- Check HDMI connection (try DisplayPort)
- Check boot order (UEFI must boot from USB)
- Check that firmware is Full ROM UEFI, not legacy ChromeOS

### If it crashes/hangs:
- Note last line printed — that identifies the failing subsystem
- Serial output (if available) gives more detail
- Most likely issue: GOP framebuffer query failing (unlikely on Intel HD)

---

## Phase 4: Validate Subsystems

Once at the `zeos>` prompt, test each subsystem:

```
zeos> help              # list available commands
zeos> mem               # show memory info
zeos> pci               # enumerate PCI devices (should see Intel HD, RTL8168, etc.)
zeos> zeros             # switch to Zeros persona (prompt turns teal)
zeos> derez             # switch to DereZ persona (prompt turns magenta)
zeos> raise             # back to Full mode
zeos> chains            # list signal chains
zeos> run programs/01_file_watcher.zp    # run a Z+ program (if available)
zeos> viz               # signal chain visualizer
```

### Expected PCI devices on CN60:
```
00:00.0 Host bridge: Intel Haswell-ULT DRAM Controller
00:02.0 VGA: Intel HD Graphics (Haswell GT1/GT2)
00:03.0 Audio: Intel Haswell HDMI Audio
00:14.0 USB: Intel 8 Series USB xHCI
00:16.0 Communication: Intel 8 Series MEI
00:19.0 Ethernet: Intel I218-V (or Realtek RTL8168)
00:1B.0 Audio: Intel 8 Series HD Audio
00:1F.0 ISA bridge: Intel 8 Series LPC
```

### What won't work yet:
- **Networking** — Z-OS only has virtio-net driver (QEMU). RTL8168/Intel I218 drivers not written.
- **NVMe/SATA** — VAULT is RAM-only. No disk persistence.
- **WiFi** — no driver
- **Audio** — no driver
- **USB storage** — no driver (USB keyboard works via UEFI emulation)

---

## Phase 5: Three-Box Configuration

### Naming convention:
```
cn60-a  — desk dev box (primary test target)
cn60-b  — second node (future: spine peer)
cn60-c  — third node (future: fleet/swarm test)
```

### RAM upgrades (recommended):
- CN60 supports 2× DDR3L SO-DIMM (1.35V)
- Upgrade to 8GB (2×4GB) or 16GB (2×8GB) per box
- More RAM = bigger VAULT = more Z+ programs in memory

### Network prep (future — when RTL8168 driver exists):
- All three on same GbE switch
- Static IPs or DHCP
- Spine registry on cn60-a, others register
- This is the first Z-OS fleet

---

## Phase 6: Development Cycle

### Workflow:
1. Edit kernel on Z13 (`/home/z13/zeos/kernel/`)
2. `make usb` (or just `make` + copy EFI to existing USB)
3. Move USB stick to CN60
4. Boot and test
5. Iterate

### Fast iteration (without reflashing USB every time):
Once USB boot works, you can:
1. Keep CN60 powered off
2. On Z13: `make` → copy `BOOTZ.EFI` to mounted USB stick
3. Safely eject
4. Boot CN60
5. ~10 seconds from code change to running on real hardware

### Even faster (future — when networking works):
- TFTP/PXE boot over network
- Edit on Z13, CN60 pulls new kernel over GbE
- No USB stick needed

---

## Phase 7: What to Build Next (on real hardware)

Priority order once basic boot works:

1. **Intel I218-V or RTL8168 driver** — networking on real hardware
   - This unlocks: spine registration from CN60, fleet communication
   - ~500-800 lines of C (register map + DMA ring buffers)

2. **AHCI/SATA driver** — disk persistence for VAULT
   - CN60 has M.2 SATA SSD
   - VAULT writes survive reboot

3. **USB mass storage driver** — load Z+ programs from USB
   - Currently programs are compiled into the kernel
   - With USB storage: `run /usb/myprogram.zp`

4. **Multi-node spine** — CN60s find each other
   - cn60-a runs spine registry
   - cn60-b and cn60-c register
   - Signal chains can span nodes

---

## Checklist Summary

```
[ ] Phase 0: Verify 3 CN60s power on with display
[ ] Phase 1: Flash MrChromebox UEFI firmware on all 3
[ ] Phase 2: Create USB boot stick from Z13
[ ] Phase 3: First boot — see zeos> prompt
[ ] Phase 4: Validate: help, mem, pci, persona switch, viz
[ ] Phase 5: Name boxes, plan RAM upgrades
[ ] Phase 6: Establish edit→build→boot workflow
[ ] Phase 7: Start RTL8168 driver for networking
```

---

## Reference

- MrChromebox firmware: https://mrchromebox.tech
- CN60 board name: `panther` (MrChromebox identifier)
- Z-OS USB image: `/home/z13/zeos/kernel/build/zeos-usb.img`
- Z-OS kernel: `/home/z13/zeos/os/build/BOOTZ.EFI`
