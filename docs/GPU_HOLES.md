# GPU holes — full map

Honest inventory of every gap between what Zeos has today and full GPU
sovereignty. Each hole gets a chain-shaped fix. No "advanced/optional" —
everything is a hole until it's covered.

## What we actually have today (2026-05-03)

- UEFI GOP framebuffer (one display, one mode, set at boot)
- EDID parsing on the GOP-selected output (commit 7b82ee3)
- PCI enumeration with a vendor name DB
- PCI class 0x03 (Display) recognised but not driven
- Audio: HDA driver works for chipset codecs; **GPU-side HDMI/DP audio
  on the GPU's own audio function is not driven**
- No native command submission. No shaders. No hot-plug. No multi-monitor.
  No multi-GPU. No accelerated anything.

## The holes, grouped

### A — Discovery and binding

| # | Hole | Chain shape |
|---|------|-------------|
| A1 | PCIe class-03 enumeration finds GPUs but no driver claims them | `CHAIN_GPU_<n>` per device, parent CHAIN_CPU |
| A2 | Vendor binding (Intel 8086, AMD 1002, NVIDIA 10de, virtio 1af4, ARM/Vivante via SoC) — no per-vendor driver shim | per-vendor `gpu_<vendor>_ops` registered the same way `net_hw_ops` is |
| A3 | Compute-only accelerators (Goya, NPUs, FPGAs with no display engine) appear as class-03 OR class-12 (signal processing). We need both | parent CHAIN_CPU, but `out: compute_result` only — no display sub-chains |
| A4 | iGPU + dGPU coexistence (Optimus / hybrid / MUX) — primary GPU vs render-only | each is its own CHAIN_GPU_n, with `prime_offload` link declared in chain_registry |
| A5 | Hot-add (Thunderbolt eGPU) | dynamic chain create/destroy on PCI hotplug event |

### B — Memory and address space

| # | Hole | Chain shape |
|---|------|-------------|
| B1 | VRAM is unmapped and unallocated | per-GPU `vram_alloc` node owning the chip's BAR0/BAR2 mapping |
| B2 | GTT / GART / GMAdr (system memory visible to GPU) untracked | `gtt_alloc` node; CFA-wrap GPU-visible buffers |
| B3 | IOMMU page tables not configured for GPU DMA — relies on identity map | `iommu_bind` node; required before `hardware_dma` is safe |
| B4 | Resizable BAR / SAM detection but never enabled | resize during pci_init if BIOS exposes the capability |
| B5 | DMA-BUF equivalent (cross-GPU buffer sharing) absent | `cfa_handle` already gives us this in concept; needs an explicit `gpu_export` route between chains |

### C — Command submission

| # | Hole | Chain shape |
|---|------|-------------|
| C1 | No ring buffer / command queue support, any vendor | per-engine sub-chain: `cmd_build → ring_push → doorbell → fence_wait` |
| C2 | No fence / sync primitives — can't tell when GPU is done | `fence_wait` node with timeout; Zixel reads completion timing |
| C3 | No preemption — long-running compute would block scanout | scheduler-side, after the kernel scheduler swap |
| C4 | MSI-X for GPU IRQs (vsync, page-flip-done, fence-signaled) not wired | per-vector handler set during gpu init; same path NICs/NVMe will use |

### D — Shader / compute

| # | Hole | Chain shape |
|---|------|-------------|
| D1 | No SPIR-V parser / IR | userspace concern initially; kernel exposes binary submission only |
| D2 | No vendor compiler (Intel: i965/iris, AMD: ACO/LLVM, NVIDIA: nouveau-codegen) | external; out of kernel scope |
| D3 | Compute kernels have no submission path | `CHAIN_GPU_n` adds `compute_dispatch` node — pairs with MDE chain so `compute.run()` in Z+ can target a GPU |
| D4 | Shared virtual memory / unified address space (modern GPUs support it) | CFA across host+GPU; one of the larger payoffs once IOMMU is up |

### E — Display engine

| # | Hole | Chain shape |
|---|------|-------------|
| E1 | Display engine and render engine treated as one — they're not | sub-chains: `CHAIN_GPU_n.render` and `CHAIN_GPU_n.display` |
| E2 | One display only (the GOP-selected one) | per-connector child chains under display: `CHAIN_DISPLAY_<gpu>_<port>` |
| E3 | Connector types not enumerated (HDMI, DP, eDP, USB-C-DP-alt, legacy DVI/VGA) | per-connector class in chain metadata |
| E4 | Encoder / connector / CRTC topology not modelled (DRM/KMS shape) | three-tier: CRTC → encoder → connector — each is a node in the display chain |
| E5 | Hot-plug detection (HPD) — pin/sense not read, no IRQ | per-connector `hpd_watch` node, IRQ-driven, MasQ records every attach/detach |
| E6 | EDID is read once at boot — never re-read on hotplug | EDID becomes a chain output of `hpd_watch`, refreshes per attach |
| E7 | DDC over I²C (the bus EDID rides on) absent | `ddc_i2c` node; required for everything beyond initial GOP read |
| E8 | DPCD (DisplayPort config data) absent | `dpcd_aux` node for DP, peer to ddc_i2c for HDMI |

### F — Mode / scan-out

| # | Hole | Chain shape |
|---|------|-------------|
| F1 | Mode set is GOP-once, never changes after boot | per-output `mode_set` resolve, vault_version bumps on change |
| F2 | Atomic modeset (multiple outputs change in lockstep) absent | `chain_registry_tick` already resolves in dependency order — atomic is a flag on the resolve |
| F3 | Page flipping / double buffering not implemented | per-output `flip_queue` node holding pending front/back |
| F4 | VSync / TE signal not wired | IRQ → bump fence node → wakes flip_queue |
| F5 | Variable refresh rate (FreeSync / G-Sync / Adaptive Sync) untouched | per-output `vrr_ctrl` node when EDID advertises capability |
| F6 | Tear-free guarantee absent | comes for free once flip_queue + vsync are real |

### G — Plane composition

| # | Hole | Chain shape |
|---|------|-------------|
| G1 | Single primary plane assumed | per-output `plane_table` with primary + overlay + cursor |
| G2 | Hardware cursor not used | dedicated `cursor` plane chain, tiny BO, position updates without recomposing |
| G3 | Overlay plane (video / YUV) absent | `overlay` plane node — pairs with hw decode when that lands |
| G4 | Per-plane scaling absent | scaler caps queried in mode_set |

### H — Color and pixel format

| # | Hole | Chain shape |
|---|------|-------------|
| H1 | LUT / gamma per CRTC unused | `gamma_lut` node, per-output |
| H2 | CSC matrix (BT.601/709/2020, RGB↔YCbCr) unused | `csc` node; required for HDR |
| H3 | HDR metadata (SMPTE 2086, CTA-861.3 InfoFrame) not emitted | `hdr_meta` node when EDID advertises HDR support |
| H4 | Wide gamut / 10-bit / 12-bit pixel formats not selected | mode_set extends pixel format; depends on plane caps |
| H5 | ICC profile awareness absent | userspace concern; kernel exposes LUT only |

### I — Power and thermal

| # | Hole | Chain shape |
|---|------|-------------|
| I1 | No DPMS — display stays at full power forever | per-output `dpms` node; suspend/resume residue |
| I2 | Panel Self Refresh (PSR) absent | per-output `psr_ctrl` if eDP advertises |
| I3 | DVFS — GPU clock/voltage state not managed | per-GPU `pstate` node |
| I4 | Thermal sensors not read; no throttle policy | hwmon-like `gpu_thermal` node, source for `pstate` |
| I5 | Suspend/resume of GPU state not implemented | scheduler swap + per-driver save/restore |

### J — Audio over HDMI / DisplayPort

| # | Hole | Chain shape |
|---|------|-------------|
| J1 | GPU-side HDA codec (Intel: PCI function 1; AMD: separate device; NVIDIA: separate device) not bound | extend HDA driver to enumerate ALL HDA controllers, not just chipset; each becomes a sub-chain of its GPU |
| J2 | ELD (EDID-Like Data for audio) absent | extracted from connector EDID, fed to GPU HDA codec |
| J3 | Sample rate / channel routing per output absent | per-DISPLAY chain has `audio_route` node when HDMI/DP carries audio |

### K — Firmware

| # | Hole | Chain shape |
|---|------|-------------|
| K1 | GuC / HuC (Intel scheduler/codec firmware) absent | embedded blob in kernel/lib/firmware/, loaded by Intel driver |
| K2 | PSP / SOS (AMD secure processor) absent | embedded blob, required before AMD GPU init |
| K3 | NVIDIA signed firmware (Turing+) absent | embedded blob; nouveau path |
| K4 | VBIOS / GOP driver shadow not parsed | option-ROM at PCI BAR; decode for fallback mode tables |

### L — Virtualization & multi-tenancy

| # | Hole | Chain shape |
|---|------|-------------|
| L1 | virtio-gpu (the QEMU paravirtual GPU) — we'd benefit from this BEFORE real HW because every QEMU test boot would have a real GPU chain | `gpu_virtio_ops` first; cheapest GPU we can fully drive |
| L2 | SR-IOV / VFs on capable GPUs absent | each VF becomes its own CHAIN_GPU |
| L3 | GPU passthrough (VFIO-style) absent | future, when Zeos hosts guests |

### M — Headless / degenerate

| # | Hole | Chain shape |
|---|------|-------------|
| M1 | Boot without any monitor must work | hpd_watch reports zero outputs, compositor renders to off-screen FB; chain still resolves |
| M2 | All monitors hot-removed at runtime must not crash | display sub-chains destroy cleanly, compositor stays LIVE |
| M3 | "Server with VGA but no GUI" — single 80x25 text path | text mode path is already there; chain registers it as a degenerate display |

### N — Cross-cutting

| # | Hole | Chain shape |
|---|------|-------------|
| N1 | No GPU-aware Z+ verbs (`gpu.dispatch(kernel, args)`, `display.flip()`) | Z+ extends after CHAIN_GPU is real, same way audio.* / fs.* landed |
| N2 | No `gpustat` shell command | trivial after chains exist |
| N3 | No GPU-aware MDE backend in CHAIN_MDE.device_select | currently MDE picks CPU only; selection logic extends per K, K, K, K |
| N4 | No telemetry export (Zixel) for GPU | each GPU sub-chain emits `out.zixel` per the contract |

## Order of attack

This is a lot. Right order is bottom-up: virtio-gpu first (we can fully
test it in QEMU), then the abstractions, then real-hardware drivers.

1. **virtio-gpu** (L1) — cheapest end-to-end win. Gives us a real
   CHAIN_GPU_0 and a real CHAIN_DISPLAY_0_VIRTIO0 in QEMU today.
2. **DRM/KMS-shaped abstractions** (E1, E4, G1) — encoder/connector/CRTC
   model lives on top of virtio-gpu first, then absorbs real drivers
   without re-design.
3. **HPD + DDC + EDID re-read on attach** (E5–E7) — runtime hot-plug.
4. **Mode-set + page flip + vsync** (F1, F3, F4) — tear-free baseline.
5. **Multi-monitor in compositor** (E2, M2) — fan-out.
6. **Intel iGPU bind** (A2, K1, B1–B3, C1–C4) — first real driver. Big.
7. **AMD GPU bind** (A2, K2) — second real driver.
8. **NVIDIA / nouveau** (A2, K3) — third.
9. **GPU HDA audio** (J1–J3) — once the GPUs are bound their audio
   functions enumerate cleanly.
10. **Compute path** (D3, N3) — GPU becomes a CHAIN_MDE backend.
11. **Power + thermal + DPMS** (I1–I5) — pairs with suspend/resume.
12. **Color + HDR** (H1–H5) — display gloss.
13. **Planes / overlay / cursor** (G1–G4) — performance gloss.
14. **VRR + PSR** (F5, I2) — premium output path.
15. **SR-IOV + passthrough** (L2, L3) — host scenarios, late.

## What every CHAIN_GPU_n must declare (contract addendum)

```
chain_create("gpu0", CHAIN_CPU, MASQ_INTERNAL)
  in:  compute_request, framebuffer_present
  out: compute_result, scanout_complete, hpd_event, gpu.zixel
  sub: chain_create("gpu0.render",  ...)
       chain_create("gpu0.display", ...)
       chain_create("gpu0.audio",   ...)   // HDA codec function
```

Per-output sub-chain:

```
chain_create("display.gpu0.HDMI1", CHAIN_GPU_0_DISPLAY, MASQ_INTERNAL)
  in:  pixel_source
  out: scanout_complete, hpd_event
  nodes: pixel_source → composite → scanout → hardware_dma
```

Compute-only accelerator (Goya, FPGA, NPU):

```
chain_create("accel0", CHAIN_CPU, MASQ_INTERNAL)
  in:  compute_request
  out: compute_result, accel.zixel
  // no display or audio sub-chains
```

## Multi-GPU + multi-scanout — implemented 2026-05-03

`gpu_virtio_init()` iterates EVERY virtio-gpu PCI function (capped at
`GPU_VIRTIO_MAX_DEVICES = 4`). Each device gets:

- its own `gpu_dev_t` slot
- its own resource-id namespace (per virtio spec; `gd->next_rid`
  starts at 1 per device)
- its own `CHAIN_GPU_<n>` + `gpu<n>.render` + `gpu<n>.display`
  sub-chains
- one `display.gpu<n>.scanout<si>` chain per enabled scanout, each
  with its OWN page-aligned backing buffer allocated from
  `pmm_alloc_contiguous` (independent of GOP fb and of every other
  scanout)

Per-display fan-out: the scheduler resolves each
`display.gpu<n>.scanout<si>` chain independently. Each chain's
`virtio_dma_resolve` copies GOP fb -> its scanout's backing then
issues `TRANSFER_TO_HOST_2D` + `RESOURCE_FLUSH` against the
scanout's resource_id. Refresh rate is gated per scanout via
`min_flush_interval_us` + `last_flush_tsc`, so a 144Hz panel does
not stall a 60Hz panel.

Canonical multi-display test:

```
make run-multigpu       # in kernel/, or
zeos run-multigpu       # from anywhere

# launches qemu-system-x86_64 with TWO -device virtio-vga.
# selftest GPU line reports:
#   GPU ........... PASS (2 device(s), 2 display(s))
# gpustat reports:
#   GPUs: 2    Displays: 2
#   gpu0  pci=1af4:1050 @ 0:3.0  scanouts=1  ready=1
#     display.gpu0.scanout0  1920x1080  edid=yes  monitor="QEMU Monitor"
#   gpu1  pci=1af4:1050 @ 0:4.0  scanouts=1  ready=1
#     display.gpu1.scanout0  1920x1080  edid=yes  monitor="QEMU Monitor"
```

QEMU's virtio-vga device exposes one scanout per device by default
and does not surface a knob to raise that to the spec's max of 16,
so the canonical multi-display test on Zeos is "two virtio-vga
devices" (one CHAIN_GPU each, one CHAIN_DISPLAY each). The driver
itself handles up to 16 scanouts per GPU, so real hardware (or a
future QEMU patch) lights up without code changes.

Today's behaviour: every connected display mirrors the GOP
framebuffer (the compositor still draws once into GOP fb; per-display
flush mirrors that into each scanout's backing). A future WM-side
change can vary the source per display without touching the driver
— only the per-display memcpy source changes.

## Honest unknowns

- We don't yet know if QEMU's virtio-gpu will give us multi-output in a
  test rig — may have to mock multi-monitor with two virtio-gpu devices.
- Goya-on-Zeos is a separate effort — the Goya driver currently runs
  under Linux. Bringing it into Zeos kernel is a port, not a wrapping.
- Real-hardware Intel/AMD drivers are weeks of work each, not days.
- Apple Silicon GPUs (Asahi-style) are out of scope — we're x86 only.
