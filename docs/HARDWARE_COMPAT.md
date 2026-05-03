# Hardware Compatibility — Zeos

What works, what doesn't, what's known to be missing. Updated as drivers
and audits land. Be honest.

---

## Required

- **x86_64 CPU**, post-2008 (we use TSC, RDRAND if present, 64-bit page tables).
  AMD or Intel both fine.
- **UEFI firmware**. BIOS-only systems do not boot Zeos. There is no MBR
  bootloader and no plan to add one. Most pre-2010 hardware is BIOS-only,
  so old laptops from that era are out unless they have UEFI in the BIOS
  setup as an option.
- **GOP-capable framebuffer**. UEFI's Graphics Output Protocol is required
  at boot — we acquire the framebuffer once before exiting boot services.
  Zeos returns from `efi_main` with an error if GOP is not present. UGA
  (the older UEFI graphics protocol) is not yet probed.
- **At least 256 MB RAM**. Heap grows from 2 MB but mbedtls + browser
  buffers want headroom.

---

## Networking

Driver probe order in `net.c`: virtio-net → e1000/e1000e → rtl8139. First
match wins. After the driver is up, DHCP is attempted; on failure all
addresses stay zero and `g_net.up = 0` so downstream calls fail cleanly.

| NIC | PCI ID | Era | Where it shows up | Status |
|---|---|---|---|---|
| virtio-net | 0x1AF4:0x1000 / 0x1041 | — | QEMU, KVM, cloud VMs | works |
| Intel 82540EM (e1000) | 0x8086:0x100E | 2005 | QEMU default Intel model | works |
| Intel 82574L | 0x8086:0x10D3 | 2008 | server boards | works (untested on real HW) |
| Intel I217-LM | 0x8086:0x153A | 2013 | Haswell laptops | works (untested on real HW) |
| Intel I218-LM | 0x8086:0x15A0 | 2014 | Broadwell laptops | works (untested on real HW) |
| Intel I219-LM/V | 0x8086:0x15A3 etc | 2015+ | Skylake+ laptops, CN60 Chromebox | works (untested on real HW) |
| Realtek RTL8139 | 0x10EC:0x8139 | 2002 | cheap consumer 100M | works in QEMU; body recv after handshake has a known issue |

Not yet supported:
- **Realtek RTL8169 / RTL8168** (gigabit). Common on circa-2008-2015
  laptops/desktops. ~500 LOC driver, larger register set than 8139.
- **WiFi**. No mac80211 stack, no firmware blobs, no probe. Wired only.
- **USB Ethernet** (CDC ECM, RNDIS, AX88179). Needs a USB stack we
  don't have.
- **Thunderbolt NICs**, dock NICs. Same — no Thunderbolt stack.

Stack notes:
- IPv4 only. No IPv6.
- Single TCP connection at a time (slot 0 in `connections[]`). No
  per-connection state machine for parallel sessions.
- No fragmentation handling (our peer must respect MTU 1500). DHCP, ARP,
  ICMP echo all work; the rest is TCP-only.
- DHCP is required — there is no static-IP UI yet. If DHCP fails on
  real hardware the kernel boots but the network is unusable until a
  `static-ip` shell command is added.
- DNS is whatever DHCP option 6 returns. No `/etc/resolv.conf`.

---

## Storage

- **NVMe** — works. Used by the OTA updater to write `BOOTZ.EFI` to the
  ESP partition. Tested in QEMU.
- **AHCI / SATA** — not yet supported. Many laptops 2008-2018 have
  SATA SSDs/HDDs; until an AHCI driver lands, the OTA updater can't
  write to those drives.
- **USB Mass Storage** — no USB stack, no driver.
- **eMMC** — not supported.

---

## Input

- **PS/2 keyboard** — works (port 0x60).
- **PS/2 mouse** — works.
- **USB HID** — no USB stack.
- **Touchscreen** — no.

---

## Crypto / Security

- **mbedTLS 3.6.4 LTS** vendored at `kernel/lib/mbedtls/`.
- **Trust store**: full Mozilla CA bundle, ~145 certs (`kernel/boot/ca_bundle.h`).
  19 certs fail to parse (18 SHA-1 self-signed legacy roots, 1 P-521
  Hungarian root); the remaining ~126 are sufficient for major public
  TLS endpoints.
- **Entropy**: RDRAND when available, mixed with TSC jitter for
  defense-in-depth. TSC-only fallback on CPUs without RDRAND.
- **Wall clock**: read from CMOS RTC at boot (double-read race
  protection), then TSC delta. Used for cert validity checks.

---

## Boot media

- **USB stick (GPT + ESP)** — primary path. `tools/zeos-build/zeos usb`
  builds the image, `zeos flash <dev>` `dd`'s it.
- **Internal storage** — possible if you can get the ESP onto it. Not
  the primary deploy path.

---

## Known gaps (filed as TODO)

- AHCI/SATA driver
- RTL8169 driver
- USB stack (any class)
- WiFi
- BIOS boot path
- UGA graphics protocol fallback (for systems without GOP)
- Static-IP shell command for DHCP-less networks
- IPv6
- TCP multi-connection
