# zeos-optane

**Detect Intel Optane devices on any Linux machine and place them into the right Zeos memory tier — interactively, with safety rails, in a Zeos-native config that carries forward when Zeos boots native.**

This is a stock-Linux installable. Zeos is **not** required to use it. Drop it on any Ubuntu / Debian / RHEL / Fedora / Arch box that has Optane hardware and it works today; the configuration it writes is the same configuration the Zeos kernel will read tomorrow.

---

## What it does

When you have Optane hardware in a machine — NVMe Optane SSDs (P-series, M-series, H-series hybrid) or Optane Persistent Memory DIMMs — the OS sees it as just another block device. `zeos-optane`:

1. **Detects** Optane devices via `lsblk` model strings, `ndctl` (when present), and `/sys/class/block` fallbacks. Read-only; no root needed.
2. **Prompts** you to choose a role for each device — slow AI memory, MasQ temporal store, VAULT hot tier, swap, or a custom mountpoint.
3. **Applies** the chosen role: formats (only with typed-device confirmation), creates the mountpoint, mounts it, appends an idempotent fstab entry (with backup), and records the result in a Zeos-native config.
4. **Re-checks** at boot via systemd, and prompts again on hot-plug via udev — if a new Optane drive appears in your machine, the journal will tell you to run `zeos-optane configure`.

## Why this matters for Zeos

Zeos is built around heterogeneous, tiered memory: DRAM at the top, bulk storage at the bottom, and a **slow unified AI-memory layer** in between for KV caches, embedding stores, weight overflow, and provenance journals. Optane sits in that gap better than anything else commodity hardware offers. This tool makes it a first-class, declarable part of a Zeos machine on day one — even before the native kernel boots — by writing config the kernel will consume:

- `/etc/zeos/optane.json` — canonical record (stable schema v1)
- `/etc/zeos/specs/optane-<id>.json` — per-device signal contract that the Zeos signal-graph builder reads as a node spec at boot

Run on Linux today. When Zeos native boots on the same machine, the Optane configuration is already there.

## Roles

| Role | Mountpoint (default) | Purpose |
| --- | --- | --- |
| `slow_ai_memory` | `/var/lib/zeos/ai-memory` | KV cache, embeddings, weight overflow tier for MDE |
| `masq_temporal` | `/var/lib/zeos/masq` | MasQ temporal-wayfinder provenance journal |
| `vault_hot` | `/var/lib/zeos/vault/hot` | VAULT hot tier between DRAM and bulk SSD |
| `swap_extension` | (swap, pri=100) | Cheap multi-hundred-GB swap extension |
| `custom` | (you choose) | Custom mountpoint; still recorded in Zeos config |
| `skip` | n/a | Leave the device alone, just record its presence |

## Install

```bash
git clone https://github.com/theBullfish/zeos.git
cd zeos/tools/zeos-optane
sudo ./install.sh
```

Installs to:
- `/usr/local/lib/zeos-optane/zeos_optane/` — Python package (stdlib only)
- `/usr/local/bin/zeos-optane` — CLI entry point
- `/etc/systemd/system/zeos-optane-detect.service` — boot-time detection
- `/etc/udev/rules.d/99-zeos-optane.rules` — hot-plug trigger
- `/etc/zeos/` — config directory skeleton

Requires: Python ≥ 3.9, `lsblk` (util-linux), `mount`, `mkfs.ext4` (or `mkfs.xfs`/`mkfs.f2fs` if you choose those). `ndctl` is optional; PMem is detected via sysfs as a fallback.

## Use

```bash
$ sudo zeos-optane scan
Detected 2 Optane device(s):
  [1] /dev/nvme1n1   INTEL SSDPED1K375GA       (375.0 GiB, nvme)
  [2] /dev/pmem0     Intel Persistent Memory   (128.0 GiB, pmem-fsdax)

$ sudo zeos-optane configure
--- Device [1/2]: /dev/nvme1n1 ---
  model: INTEL SSDPED1K375GA
  size:  375.0 GiB
  kind:  nvme

What should this device be used for?
  1) Slow AI memory tier (recall, KV cache, embeddings, weight overflow)
     Treat this device as the slow tier of unified AI memory. ...
  2) MasQ temporal store (provenance / recall journal)
  3) VAULT hot tier (between DRAM and bulk SSD)
  4) Swap / extended memory (high-priority swap)
  5) Custom mountpoint
  6) Leave alone

  Choice [1]: 1

Device:     /dev/nvme1n1  (INTEL SSDPED1K375GA)
Role:       slow_ai_memory  (Slow AI memory tier ...)
Mountpoint: /var/lib/zeos/ai-memory
Filesystem: ext4
Options:    defaults,noatime,lazytime
Steps:
  ! [1] Create ext4 filesystem on /dev/nvme1n1
        $ mkfs.ext4 -F -L zeos-aimem -E lazy_itable_init=1,lazy_journal_init=1 /dev/nvme1n1
  - [2] Ensure mountpoint /var/lib/zeos/ai-memory exists
        $ mkdir -p /var/lib/zeos/ai-memory
  - [3] Mount ext4 on /var/lib/zeos/ai-memory
        $ mount -t ext4 -o defaults,noatime,lazytime /dev/nvme1n1 /var/lib/zeos/ai-memory
fstab:     LABEL=zeos-aimem  /var/lib/zeos/ai-memory  ext4  defaults,noatime,lazytime  0  2

Apply this plan? (yes/no) [no]: yes

Applying plan for /dev/nvme1n1 (slow_ai_memory):
  $ mkfs.ext4 ...
!!  DESTRUCTIVE STEP  !!
    To confirm, type the device path exactly: /dev/nvme1n1
    > /dev/nvme1n1
  ...
Done.
```

Non-interactive variant:

```bash
sudo zeos-optane apply --device /dev/nvme1n1 --role slow_ai_memory --no-dry-run --yes
```

## Safety

- **Dry-run by default.** `apply` does nothing without `--no-dry-run`. `configure` always asks before applying.
- **Typed-device confirmation.** Every destructive step (mkfs / mkswap) requires you to type the device path back exactly. No tab-completion shortcut formats the wrong drive.
- **Existing filesystems are preserved.** A device that already has a filesystem is mounted as-is; reformatting requires `--force-format`.
- **/etc/fstab is backed up** to `/etc/fstab.zeos-optane.bak.<timestamp>` before any append, and entries are deduplicated by (source, mountpoint).
- **Detection is read-only** and runs fine as a normal user.

## Uninstall

```bash
sudo ./uninstall.sh
```

Removes the package, CLI, systemd unit, and udev rule. **Leaves `/etc/zeos/` and existing fstab entries alone** — your Optane config is data and survives the uninstall, ready for the next install or for Zeos native.

## Files

```
tools/zeos-optane/
├── README.md                              this file
├── pyproject.toml                         package metadata
├── install.sh                             root installer
├── uninstall.sh                           root uninstaller
├── zeos_optane/                           stdlib-only Python package
│   ├── __init__.py / __main__.py
│   ├── cli.py                             argparse CLI (scan/configure/apply/...)
│   ├── detect.py                          NVMe + PMem detection
│   ├── roles.py                           Zeos role taxonomy
│   ├── plan.py                            plan builder
│   ├── apply.py                           plan executor with safety rails
│   └── config.py                          /etc/zeos/optane.json + signal contracts
├── systemd/zeos-optane-detect.service     boot-time check
├── udev/99-zeos-optane.rules              hot-plug trigger
└── tests/test_detect.py                   detection unit tests (with fixtures)
```

See also: `docs/OPTANE_TIER.md` for the architecture spec.
