# Real multicore in Zeos

Find-the-Lie target: not "APs resolve chains in parallel like Linux."
The best multicore implementation any OS has done — because the chain
graph carries metadata Linux doesn't have.

## Why Linux SMP is the wrong shape

- Static per-CPU partition at scheduler tick. Migration is heuristic.
- Lock-heavy (RCU, futex, spinlocks). Every shared structure is a
  contention point.
- IPC across cores goes kernel→userspace→kernel = boundary tax.
- Core affinity is operator-set via `taskset`. The kernel doesn't
  learn.
- No provenance trail. Concurrency bugs need `strace + perf + lucky guess`.
- No "what is the type of this signal" awareness — the scheduler
  treats threads as opaque.

## What Zeos has that Linux doesn't

- **Chains are typed signals.** Every chain declares input_type and
  output_type. MDE auto-routes by type match.
- **B3 belief per chain.** alpha/beta updated every resolve. Failure
  rate is first-class.
- **MasQ journal.** Every state change recorded with timestamp,
  prior version, who-changed-what.
- **CFA addresses.** Every chain has a fractal address. Locality is
  derivable from the address tree.
- **Per-CPU state struct.** Already allocated by smp_init.
- **Per-CPU TLB shootdown via IPI.** Already wired (commit e285d4a).
- **MDE topo-sort.** Already runs.

These primitives let us do things Linux structurally cannot.

## The seven pillars

### 1. Work-stealing per-core ready queues
Each core has its own ready queue (chain ids waiting for resolve). An
empty core steals from a busy neighbor's tail. No static partition;
load balances naturally.

### 2. Per-(chain × core) B3 matrix
chain_t.b3_alpha and chain_t.b3_beta become 8-element arrays indexed
by core id. The scheduler can read "CHAIN_NET_TX has b3_beta = 0.02
on core 2 but 0.31 on core 0" and route accordingly. Linux can't do
this because Linux doesn't know what a "chain" is.

### 3. Adaptive affinity
A small loop in the scheduler periodically checks: for each chain,
does any core have a markedly better B3 than the current pinned core?
If yes, migrate. The OS LEARNS the right pinning.

### 4. Locality groups
chain_t gains a `locality_group` field (uint16_t). Chains sharing a
group are co-scheduled on the same core. Use it for:
- (CHAIN_NET_TX, CHAIN_NET_RX, CHAIN_NET_TLS) → group "net"
- (CHAIN_BLOCK, CHAIN_FS_EVENT) → group "fs"
- (CHAIN_AUDIO, CHAIN_VIDEO_IN, CHAIN_NOTIFY) → group "media"
The scheduler respects groups when stealing.

### 5. Pipeline-aware MDE routing
When chain A emits and MDE auto-routes to chain B (by type match), B
runs on whichever core ALSO has the lowest cost given:
- Same core as A → cache hot, prefer if free
- Core in same locality group → second prefer
- Any idle core with same NUMA node (when we have NUMA) → third
- Any idle core → fallback
This is basically what Linux's "CFS wake balance" tries to do, except
Zeos has structural type metadata to make it deterministic instead of
heuristic.

### 6. Power-aware idle
Idle cores enter HLT and the LAPIC timer is set to wake on either
- a steal-candidate appearing in another core's queue, or
- the next chain's resolve_interval_ticks.
Hot cores stay hot. Cold cores actually sleep.
Thermal throttle: when ACPI _TMP exceeds a threshold, route hot chains
away from the hot core to a cooler one.

### 7. MasQ cross-core handoff record
Every time a chain resolves on a core different from the one that
emitted its input, append a journal entry: "[xfer] chain N input
emitted on core 2, resolved on core 1, latency K us." Debugging
concurrency becomes reading the trail, not guessing.

## Reverse-engineering — what's needed in order

After the AP wedge unblock (in flight):

| # | Item | Touches | Effort |
|---|------|---------|--------|
| 1 | Per-CPU ready queue + work-stealing | smp.c, scheduler.c | medium |
| 2 | Per-(chain × core) B3 matrix | chain.h, chain.c, scheduler.c | small |
| 3 | Adaptive affinity loop | scheduler.c | small |
| 4 | Locality groups | chain.h, chain.c, chain_registry.c | small |
| 5 | Pipeline-aware routing | mde.c | medium |
| 6 | Power-aware idle | smp.c, lapic.c, ec.c (thermal) | medium |
| 7 | Cross-core handoff records | scheduler.c, block_chain.c | small |

Demos / benchmarks (after pillar 1 lands):
- 4 cores resolving 4 different chains in parallel — selftest line
  shows non-zero `chains_resolved` on all APs, work-stealing in action.
- Synthetic-load test: 1000 chains, 4 cores, vs Linux on same QEMU.
  Linux scales sub-linearly for typed-message workloads;
  Zeos should scale near-linearly because there's no userspace
  boundary tax.

## What "best multicore" looks like in the README

> On the same hardware, the same workload (10k typed messages routed
> through a five-stage chain pipeline) runs in N seconds on Linux
> with 4 cores and N/3 seconds on Zeos with 4 cores. The difference
> isn't optimization — it's that Zeos's scheduler reads the chain
> graph as authoritative, while Linux's reads thread state as a
> hint.

That's the lie made true.
