# Dom/Sub — Cooperative Multi-Chip PCIe Compute Fabric

> Xeon Phi, but the coprocessors actually know each other and one of them
> is in charge.
>
> **Date**: July 24, 2026
> **Status**: Specification — not yet implemented
> **Owner**: kernel/boot (pci.c, hotplug.c, gpu_goya.c, chain_registry.c, smp.c)
> **Naming**: "Dom/Sub" is the working name — deliberately informal, kept
> because it's funny and it's unambiguous. Swap it later if it stops being
> funny. The technical semantics below don't depend on the name.

---

## Core Principle

Multiple ARM co-processor chips (Goya-class today, any PCIe-attached ARM
SoC later) hot-plug into a Zeos host and recognize each other as **the same
team**, not competitors. One chip is elected **Dom** — it gets the
high-priority THINK work. Every other chip in the group is a **Sub** — it
gets the small, background, incidental work. The relationship is
cooperative and non-adversarial: a Sub isn't "beaten," it's just not the
best fit for THINK traffic *right now*, and the group re-elects whenever
membership changes.

This is not a new bus, not a new wire protocol, not new silicon. It's a
coordination layer built entirely on infrastructure Zeos already has:

| Need                         | Already exists                          |
|-------------------------------|------------------------------------------|
| Detect a chip attaching live  | `hotplug.c` — `CHAIN_HOTPLUG_PCI`, poll-diff, `HOTPLUG_EVT_PCI_ATTACH/DETACH` |
| Talk to the chip              | `gpu_goya.c` — BAR mapping, MSI-X completion ISR, register read/write, already multi-instance (`GOYA_MAX_DEVICES=8`) |
| Route work to a specific chip | `chain_registry.c` / `chain_t.affinity` + `smp_chain_owner()` pattern **exists for CPU cores, not yet wired for chips** (correction below) |
| Priority-ish scheduling       | `chain_t.watchdog_timeout_us`, `resolve_interval_ticks`, B3 belief |

Dom/Sub adds: a **cohort** concept (which chips belong to the same
cooperative group), an **election** (who's Dom right now), and a
**priority class** on chains (THINK vs BACKGROUND) that determines which
cohort member a chain's affinity gets pinned to.

---

## Vocabulary

- **Chip** — one PCIe-attached ARM co-processor device (a Goya card today;
  the vendor/device allowlist is meant to grow).
- **Cohort** — the set of chips Zeos has decided are teammates. MVP: one
  cohort, global, every recognized chip joins it. Future: multiple cohorts
  (e.g. two independent task pools) — out of scope for v1, see Open
  Questions.
- **Dom** — the one chip in a cohort currently elected to receive
  THINK-class work. Exactly one Dom per cohort, or zero if the cohort is
  empty.
- **Sub** — every other chip in the cohort. Receives BACKGROUND-class
  work. Zero or more Subs.
- **THINK-class chain** — a chain tagged high-priority: the actual
  cognitive/inference workload the user is waiting on.
- **BACKGROUND-class chain** — everything else routed to the cohort:
  prefetch, speculative work, telemetry rollups, low-urgency batch jobs —
  "little stuff and incidental shit."

---

## 1. Chip Class Registration

A chip is anything matching a registered (vendor_id, device_id) pair in a
new small table, analogous to how `gpu_goya_init()` already matches Goya's
PCI ID during boot enumeration:

```c
/* dom_sub.h */
typedef struct {
    uint16_t vendor_id;
    uint16_t device_id;
    const char *label;     /* "goya", future: "qualcomm-qXA", etc. */
} chip_class_t;

static const chip_class_t CHIP_CLASSES[] = {
    { 0x1DA3, 0x0001, "goya" },   /* confirmed real hardware, fleet-tested */
    /* future entries appended here, never removed -- append-only per BIBLE G1 */
};
```

`dom_sub_is_cooperative_chip(struct pci_device *d)` walks this table the
same way `gpu_goya_init()` already walks `pci_enumerate()`'s results. This
keeps the allowlist explicit and auditable instead of accepting "anything
that looks ARM-ish."

---

## 2. Discovery — riding the existing hotplug chain

No new detection mechanism. `hotplug.c` already runs `CHAIN_HOTPLUG_PCI`
every tick, diffing the PCI bus and emitting `HOTPLUG_EVT_PCI_ATTACH` /
`HOTPLUG_EVT_PCI_DETACH` into its ring buffer (`hotplug.c:34-70`). Dom/Sub
adds one new chain, `CHAIN_DOM_SUB_COHORT`, that drains that ring and:

- On `HOTPLUG_EVT_PCI_ATTACH` where the device matches `CHIP_CLASSES`:
  add the chip to the cohort, run the identify handshake (§3), trigger a
  re-election (§4).
- On `HOTPLUG_EVT_PCI_DETACH` for a chip currently in the cohort: remove
  it, requeue any in-flight work that was pinned to it (§5), trigger a
  re-election.
- Boot-time chips (already enumerated by `gpu_goya_init()` before hotplug
  starts polling) are added to the cohort during `dom_sub_init()`, once,
  at the same point in `main.c` where `gpu_goya_init()` already runs.

This is exactly "add a chip by PCIe" — plug it in, the existing hotplug
poll-diff notices it within one tick, cohort membership updates, election
re-runs, no reboot required.

---

## 3. Identify handshake

Once a chip joins the cohort, Zeos needs enough information from it to
score it in the election. This reuses the register-read pattern already
in `gpu_goya.c` (`goya_r32`/`goya_w32` over the mapped BAR):

```c
typedef struct {
    uint32_t fw_version;
    uint32_t dram_mb;        /* on-chip DRAM, e.g. Goya's 8GB fleet cards */
    uint32_t thermal_margin; /* whatever the chip's own sensor reports, 0 if unknown */
    uint32_t bench_score;    /* see below -- 0 if not yet benchmarked */
} chip_identity_t;
```

MVP: `fw_version` and `dram_mb` are read directly from known-good
registers (Goya's are already decoded per the Phase 1 register map work).
`bench_score` starts at 0 (unbenched) and is filled in lazily by the
first real THINK-class job's measured throughput — this avoids inventing
a synthetic benchmark workload before real workloads exist to calibrate
against. Until a chip has a real `bench_score`, it's eligible for
election but loses ties (see §4).

---

## 4. Election

Runs whenever cohort membership changes (attach, detach) or when the
current Dom's `bench_score` is beaten by a Sub's by a real, measured
margin (not a coin-flip on noise — needs a defined hysteresis, see below).

**MVP scoring, highest wins:**

```
score = bench_score * 1000 + dram_mb
```

(`bench_score` dominates once chips have run real work; `dram_mb` is
purely a cold-boot tiebreak before anything has been benchmarked — an
honest placeholder, not a claim that DRAM size predicts think-quality.)

**Tiebreak:** lowest PCI bus/dev/func address wins (deterministic,
matches how `pci_enumerate()` already orders devices — no randomness).

**Hysteresis (real requirement, not optional):** a Sub does not unseat
the current Dom on a single close score. Require the challenger's score
to exceed the incumbent Dom's by a fixed margin (e.g. 10%, tune once real
`bench_score` data exists) sustained across N consecutive measurements.
Without this, two evenly-matched chips would flap Dom/Sub back and forth
every time their scores cross, which would thrash affinity and cancel any
benefit of having a Dom at all.

**On election result changing:** log it (this is a real state transition,
BIBLE G2 — needs a timestamp and the reason, not just a silent flip), and
re-point THINK-class chain affinity at the new Dom (§5). In-flight work
on the outgoing Dom is NOT killed — it finishes where it's running; only
*future* THINK-class dispatch moves.

---

## 5. Task classification & routing

Two new chain tags, set at chain-creation time by whatever subsystem
enqueues cooperative-chip work (the MDE inference path, most likely):

```c
typedef enum {
    DOM_SUB_CLASS_THINK,       /* high-priority: user is waiting on this */
    DOM_SUB_CLASS_BACKGROUND,  /* everything else -- prefetch, batch, telemetry */
} dom_sub_class_t;
```

Routing extends the existing `chain_t.affinity` field and
`smp_chain_owner()` fallback pattern (`smp.c:771-778`) — same mechanism as
CPU-core chain ownership, no new scheduling *primitive* needed. **Correction
2026-07-24** (paradigm-conformance audit, `specs/PARADIGM_CONFORMANCE_AUDIT.md`
§4): this is currently CPU-affinity only — no code today sets `affinity` on a
Goya chain, and `gpu_goya.c` has no CFA wrapping of its BAR/DMA handles either.
The pattern is right to extend, but §1-7 of this spec's "already exists"
framing for chip-level affinity was aspirational, not accurate as of tonight —
Q.5 (below) needs to actually wire per-chip affinity into `gpu_goya.c`'s chain
registration, not just point `chain_t.affinity` at a chip index and assume the
rest works:

- `DOM_SUB_CLASS_THINK` chains: `affinity` is pinned to the current Dom's
  chip slot. If the Dom changes mid-flight (§4), only chains created
  *after* the change follow the new Dom.
- `DOM_SUB_CLASS_BACKGROUND` chains: `affinity` round-robins across
  current Sub slots (empty cohort or single-chip cohort: falls back to
  whatever chip exists, Dom included — a lone chip is both Dom and Sub by
  necessity, there's no such thing as an idle chip in a one-chip cohort).
- Zero chips in cohort: both classes fall back to whatever compute path
  Zeos already uses without Dom/Sub (host CPU / DispatchEngine / whatever
  the caller's non-cooperative fallback is) — Dom/Sub is additive, never
  a hard dependency.

---

## 6. Detach / failure handling

`HOTPLUG_EVT_PCI_DETACH` on a cohort member:

- If it was a Sub: remove from cohort, re-election runs (cheap — no Dom
  change unless the Sub was somehow scoring higher, which can't happen
  post-detach anyway since it's gone). BACKGROUND-class round-robin drops
  that slot.
- If it was the Dom: remove from cohort, **immediate** re-election among
  remaining members (no hysteresis delay on this path — the old Dom is
  physically gone, there's nothing to debounce against). Any THINK-class
  chain that was mid-resolve on the departed chip is marked `CHAIN_ERROR`
  by the existing watchdog path (same mechanism already verified for
  hung resolves generally) and is eligible for the caller's normal
  retry/requeue logic — Dom/Sub does not invent a new failure-recovery
  primitive, it inherits the one that already exists.

---

## 7. What this spec does NOT cover (explicitly out of scope for v1)

- **Multiple independent cohorts.** MVP is one global cohort. If Brad
  wants two separate chip pools working on unrelated task sets later,
  that needs a cohort ID threaded through the chain tag and the hotplug
  match table — straightforward extension, not designed here because
  there's no concrete second use case yet to design against.
- **Cross-chip work migration.** If Dom changes mid-flight, in-flight
  THINK work on the old Dom finishes there, it does not get live-migrated
  to the new Dom. Live migration of in-flight accelerator state is a much
  bigger feature (checkpoint/restore of whatever the chip was doing) and
  isn't needed for the "best one gets the job" ask as stated.
- **Real bench_score methodology.** Deferred until real THINK-class
  workloads exist to measure — inventing a synthetic Goya benchmark
  before there's a real one to validate against would be exactly the
  kind of unmeasured, unobserved claim BIBLE's One Law rejects.
- **Non-Goya chip classes.** The `CHIP_CLASSES` table has one entry today
  because that's the only hardware in hand. Adding a Qualcomm or other
  ARM SoC class is a one-line table addition once that hardware exists to
  test against, not a design change.

---

## Implementation order (for BUILD_MAP.md tracking)

1. `dom_sub.h` / `dom_sub.c` — chip class table, cohort struct, identify
   handshake (reuses `gpu_goya.c` register access).
2. `CHAIN_DOM_SUB_COHORT` chain wired into `hotplug.c`'s attach/detach
   ring, boot-time cohort seeding in `main.c` alongside `gpu_goya_init()`.
3. Election (§4) — scoring, hysteresis, re-election triggers, state-change
   logging.
4. `dom_sub_class_t` tagging + affinity routing (§5) — the actual THINK
   vs BACKGROUND split.
5. Detach/failure handling (§6) — inherits existing watchdog/CHAIN_ERROR
   path, should be close to free once §2-4 exist.
6. Real hardware verification: two Goya cards in the same host, confirm
   election picks one, confirm THINK-class chain affinity actually lands
   on the elected Dom, confirm unplugging the Dom re-elects the Sub within
   one hotplug poll tick — measured via serial log + chain affinity
   inspection, not asserted from source.
