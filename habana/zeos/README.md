# habana/zeos — Zeos-side Goya bypass

The Zeos kernel drives Habana silicon natively when a Zeos box boots. Same wire format as `habana/linux/habana_proof`, just compiled into the kernel instead of a Linux userspace binary.

## Files in this folder

- `mme_proof.zp` — Tier 1 Z+: the smoke / roof / real ladder as a readable signal graph
- `mme_proof_t3.zp` — Tier 3 Z+: the same graph condensed for expert reading

Z+ is the project's signal-graph language. T1 is verbose with diagrams; T3 is dense. Same compile, different cognitive load. A high-school student reads T1 to understand the bypass; a kernel hacker reads T3 like a schematic.

## Files elsewhere

The kernel C lives where every other Zeos driver lives — `kernel/boot/`. That's project convention (see `kernel/boot/gpu_nvidia.c`, `kernel/boot/gpu_virtio.c`, etc., all in the same directory).

- `kernel/boot/gpu_goya.h` — Public bring-up surface (PCI, BARs, MSI-X, firmware FIT, TPC ring, accessors)
- `kernel/boot/gpu_goya.c` — Bring-up implementation; calls `goya_mme_run_ladder_all()` at end of `gpu_goya_init`
- `kernel/boot/gpu_goya_mme.h` — MME programmer public surface
- `kernel/boot/gpu_goya_mme.c` — MME descriptor packet builder + submitter + smoke/roof/real ladder

`gpu_goya_mme.c` is the load-bearing piece — same 14-packet command stream as the Linux binary, just submitted through Zeos's TPC ring instead of `HL_IOCTL_CS`.

## How it runs

When a Zeos box boots, `gpu_goya_init` walks the PCI bus, brings up every detected HL-1000, then calls `goya_mme_run_ladder_all()`. The boot log prints one line per step per card:

```
[goya] cards=1 fw=embedded
  goya-mme smoke .......... passed (8x8x8 FP16, 87 us)
  goya-mme roof ........... passed (3812 GFLOPS observed, peak ~5000)
  goya-mme real ........... passed (Q*Kt 64x64x64 FP16, 22 us) -- softmax+V follow w/ tpc_kernel
```

If smoke ever fails on real silicon, the architectural claim is suspect and the boot log says exactly that. See `habana/GOYA_BYPASS.md` for what that means and how we'd re-plan.

## How to read the Z+ files

Open `mme_proof.zp` first. The whole proof ladder is in plain English with a flow diagram at the bottom. If you want the dense version, `mme_proof_t3.zp` is the same proof in 50 lines.

These files describe the *intent* of the bypass in the language the OS itself is built around. The kernel C in `kernel/boot/` is what actually runs.
