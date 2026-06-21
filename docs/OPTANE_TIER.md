# The Optane Tier

**Where Intel Optane fits in the Zeos memory hierarchy, and how `zeos-optane` integrates it on stock Linux today and Zeos native tomorrow.**

*Codex Labs LLC — 2026*

---

## The gap Optane fills

Zeos's memory hierarchy, on any machine that has it:

```
  fastest, smallest                                                   slowest, largest
  +----------+-------+--------------+---------------+---------------+
  |   Reg    | DRAM  |   Optane     |   NAND SSD    |   Bulk HDD    |
  |          |       |   (the gap)  |               |               |
  +----------+-------+--------------+---------------+---------------+
  ~ns         ~80ns   ~300ns/~10us   ~80us           ~10ms
```

Between DRAM and NAND there is a roughly two-orders-of-magnitude gap in latency and durability. Intel Optane (3D XPoint) — both the persistent-memory DIMMs (Apache Pass / Barlow Pass) and the NVMe drives (P-series, M-series, H-series) — is the only commodity device that lives in that gap. It is byte-addressable when used as DAX-mapped persistent memory, latency in the low microseconds, with write endurance an order of magnitude better than NAND.

Zeos's MDE, MasQ, and VAULT components all benefit from a tier in this gap. Without an explicit tier, those components either fight for DRAM or get pushed onto NAND and lose orders of magnitude of throughput. With an Optane tier declared, they can place hot-but-large state where it belongs.

## What Optane gets used for in Zeos

| Role key | Component | Workload |
| --- | --- | --- |
| `slow_ai_memory` | MDE | KV cache, embedding store, weight overflow tier when DRAM is tight |
| `masq_temporal` | MasQ | Append-heavy provenance / recall journal; image-based history |
| `vault_hot` | VAULT | Hot tier between DRAM and bulk SSD for frequently-touched modules / packages / state |
| `swap_extension` | kernel | Priority-100 swap — cheap multi-hundred-GB headroom for occasional spills |
| `custom` | (user) | User-named mountpoint; still recorded as a signal-graph node |
| `skip` | — | Device left alone but recorded so the system knows it exists |

Roles are declarative. The user picks one per device; the system materializes it.

## How a device gets placed

The flow is the same on stock Linux today (under the POSIX compat layer) and on Zeos native tomorrow:

```
  detect ──→ prompt ──→ plan ──→ confirm ──→ apply ──→ persist ──→ signal-graph node
```

1. **detect** — `lsblk` model strings (`OPTANE`, `INTEL SSDPED*`, `INTEL MEMPEK*`, etc.) plus `ndctl` namespace listings plus `/sys/class/block/pmem*` fallback.
2. **prompt** — interactive menu of the six roles, one per detected device.
3. **plan** — generate the exact `mkfs` / `mkdir` / `mount` / `mkswap` / fstab steps.
4. **confirm** — typed-device confirmation gate on every destructive step.
5. **apply** — run the steps; back up `/etc/fstab` first; write idempotently.
6. **persist** — record the result in `/etc/zeos/optane.json` and emit a per-device signal contract under `/etc/zeos/specs/optane-<id>.json`.

## Persistent state

Two files (per device, in the case of contracts) carry the configuration forward:

### `/etc/zeos/optane.json`

The canonical record. Schema version 1:

```json
{
  "version": 1,
  "host": "<hostname>",
  "updated_at": "<iso8601>",
  "devices": [
    {
      "path": "/dev/nvme1n1",
      "kind": "nvme",
      "model": "INTEL SSDPED1K375GA",
      "size_bytes": 375083606016,
      "serial": "PHKE000000000",
      "wwid": "eui.01000000010000005CD2E4F1F4F40000",
      "zeos_role": "slow_ai_memory",
      "signal_contract": "optane.slow_ai_memory.v1",
      "mountpoint": "/var/lib/zeos/ai-memory",
      "fstype": "ext4",
      "mount_options": "defaults,noatime,lazytime",
      "fstab_label": "zeos-aimem",
      "applied_at": "<iso8601>",
      "applied_by": "zeos-optane 0.1.0"
    }
  ]
}
```

### `/etc/zeos/specs/optane-<id>.json`

A per-device signal contract — the format Zeos native reads at boot to populate signal-graph nodes. Each Optane device shows up as a first-class node in the graph with an explicit role and bound mount point:

```json
{
  "contract": "optane.slow_ai_memory.v1",
  "device": { "path": "/dev/nvme1n1", "kind": "nvme", ... },
  "role": "slow_ai_memory",
  "mount": { "mountpoint": "/var/lib/zeos/ai-memory", "fstype": "ext4", ... },
  "applied_at": "<iso8601>",
  "applied_by": "zeos-optane 0.1.0",
  "schema_version": 1
}
```

## Forward compatibility with Zeos native

The whole point of writing config in this format on stock Linux is that Zeos native will consume the same files at the same paths, with no migration step:

| Today (Linux host) | Tomorrow (Zeos native) |
| --- | --- |
| `lsblk` / `ndctl` / sysfs detection | Zixel + bus discovery |
| `mkfs` / `mount` / fstab | Signal-graph node bind |
| `/etc/zeos/optane.json` | Read by kernel signal-graph builder at boot |
| `/etc/zeos/specs/optane-*.json` | Loaded as device signal contracts |
| `/var/lib/zeos/ai-memory` | Mapped to MDE slow-tier endpoint |
| `/var/lib/zeos/masq` | Mapped to MasQ store |
| `/var/lib/zeos/vault/hot` | Mapped to VAULT hot-tier endpoint |

Run `zeos-optane` once on each machine. When Zeos boots native on that machine, the Optane tier is already declared.

## Why typed-device confirmation, even with --yes

`zeos-optane` deliberately prompts for the device path on every destructive step (`mkfs`, `mkswap`), even when the user passed `--yes`. This is structural: `--yes` is a session-level "I intend to apply destructive changes," but the typed confirmation is a per-device "this specific device is the right one." A mistyped `--device` flag, a tab-complete that grabbed the wrong drive, or muscle memory from a different machine cannot survive having to type the device path back exactly. The cost is one extra second of input. The benefit is that no Zeos tool will ever silently format the user's primary disk.

## See also

- `tools/zeos-optane/` — the installable
- `tools/zeos-optane/README.md` — install / use / safety
- `docs/HARDWARE_DISCOVERY.md` — the broader self-discovery model
- `specs/HARDWARE_ABSTRACTION.md` — signal-graph abstraction Zeos uses
