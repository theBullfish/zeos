"""Role definitions for Optane devices in the Zeos memory tier.

A *role* answers the user's question: "what should this device be used for?".
Each role carries the configuration knobs needed to materialize it as a mount
point, swap area, or raw signal-graph endpoint — both on stock Linux today and
under Zeos native tomorrow. The role names here are the names the Zeos kernel
uses internally, so the JSON written by ``zeos-optane`` is forward-compatible.
"""
from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class Role:
    key: str                        # canonical Zeos role name; written into config
    label: str                      # short display label
    description: str                # one-paragraph user-facing description
    default_mountpoint: str         # "" for swap / raw devdax
    default_fstype: str             # "" for swap / raw devdax
    mount_options: str              # fstab field 4
    fstab_dump: int = 0
    fstab_pass: int = 2
    needs_filesystem: bool = True
    is_swap: bool = False
    signal_contract: str = ""       # specs/ contract this role emits

    def display(self, index: int) -> str:
        return f"  {index}) {self.label}\n     {self.description}"


# Order here is the order presented in the interactive menu. Keep "skip" last.
ROLES: list[Role] = [
    Role(
        key="slow_ai_memory",
        label="Slow AI memory tier (recall, KV cache, embeddings, weight overflow)",
        description=(
            "Treat this device as the slow tier of unified AI memory. MDE uses it "
            "for KV caches, embedding stores, and overflow weights when DRAM is "
            "tight. Low write amplification; survives reboots."
        ),
        default_mountpoint="/var/lib/zeos/ai-memory",
        default_fstype="ext4",
        mount_options="defaults,noatime,lazytime",
        signal_contract="optane.slow_ai_memory.v1",
    ),
    Role(
        key="masq_temporal",
        label="MasQ temporal store (provenance / recall journal)",
        description=(
            "Back the MasQ temporal wayfinder with this device. Append-heavy, "
            "byte-addressable when fsdax is available — perfect fit for image-"
            "based provenance."
        ),
        default_mountpoint="/var/lib/zeos/masq",
        default_fstype="ext4",
        mount_options="defaults,noatime",
        signal_contract="optane.masq_temporal.v1",
    ),
    Role(
        key="vault_hot",
        label="VAULT hot tier (between DRAM and bulk SSD)",
        description=(
            "Place this device in VAULT's hot tier so frequently-touched modules, "
            "packages, and state land here before the bulk tier."
        ),
        default_mountpoint="/var/lib/zeos/vault/hot",
        default_fstype="ext4",
        mount_options="defaults,noatime",
        signal_contract="optane.vault_hot.v1",
    ),
    Role(
        key="swap_extension",
        label="Swap / extended memory (high-priority swap)",
        description=(
            "Configure this device as priority-100 swap. Cheap way to hand the "
            "kernel an extra few hundred GB of slow-RAM-equivalent — useful for "
            "training jobs that occasionally spill."
        ),
        default_mountpoint="",
        default_fstype="",
        mount_options="pri=100",
        fstab_dump=0,
        fstab_pass=0,
        needs_filesystem=False,
        is_swap=True,
        signal_contract="optane.swap_extension.v1",
    ),
    Role(
        key="custom",
        label="Custom mountpoint",
        description=(
            "You name the mountpoint and filesystem. The device is still recorded "
            "in the Zeos config so the signal graph knows it exists."
        ),
        default_mountpoint="",
        default_fstype="ext4",
        mount_options="defaults,noatime",
        signal_contract="optane.custom.v1",
    ),
    Role(
        key="skip",
        label="Leave alone",
        description=(
            "Don't touch this device. It will still show up in 'zeos-optane "
            "status' so you can come back to it later."
        ),
        default_mountpoint="",
        default_fstype="",
        mount_options="",
        needs_filesystem=False,
        signal_contract="",
    ),
]


ROLES_BY_KEY = {r.key: r for r in ROLES}


def role_choices() -> str:
    return "\n".join(r.display(i) for i, r in enumerate(ROLES, 1))


def role_for_index(idx: int) -> Role:
    if idx < 1 or idx > len(ROLES):
        raise ValueError(f"role index out of range: {idx}")
    return ROLES[idx - 1]


def role_for_key(key: str) -> Role:
    try:
        return ROLES_BY_KEY[key]
    except KeyError as exc:
        raise ValueError(f"unknown role: {key}") from exc
