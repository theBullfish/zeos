"""Build an apply-plan from an Optane device + chosen role.

A plan is a list of shell steps annotated with whether they are destructive and
whether they require root. ``apply.py`` walks this list and prints / executes
each step, so the plan can be inspected before any state is changed.

Plans are deliberately conservative:

  * Devices with an existing filesystem are *never* reformatted unless the
    caller passes ``force_format=True``. The default path mounts the existing
    filesystem instead.
  * fstab is appended to (idempotently) rather than rewritten; ``apply.py``
    backs the file up before any write.
  * For PMem in fsdax mode, ``-o dax`` is included automatically.
  * For PMem in devdax mode, no filesystem is created — the device is recorded
    in the Zeos config as a raw signal-graph endpoint.
"""
from __future__ import annotations

import os
from dataclasses import dataclass, field
from typing import Optional

from .detect import OptaneDevice
from .roles import Role


@dataclass
class Step:
    cmd: list[str]
    description: str
    destructive: bool = False
    requires_root: bool = True


@dataclass
class Plan:
    device: OptaneDevice
    role: Role
    mountpoint: str
    fstype: str
    mount_options: str
    steps: list[Step] = field(default_factory=list)
    fstab_line: str = ""              # appended after steps run, by apply.py
    notes: list[str] = field(default_factory=list)

    def is_noop(self) -> bool:
        return not self.steps and not self.fstab_line


def _mkfs_args(fstype: str, device_path: str, label: str) -> list[str]:
    if fstype == "ext4":
        # -F = don't prompt if the device looks busy or has an old fs signature
        # (apply.py has already gated this with typed confirmation upstream).
        # -E lazy_itable_init = fast format; safe for slow tier.
        return ["mkfs.ext4", "-F", "-L", label, "-E", "lazy_itable_init=1,lazy_journal_init=1", device_path]
    if fstype == "xfs":
        return ["mkfs.xfs", "-f", "-L", label, device_path]
    if fstype == "f2fs":
        return ["mkfs.f2fs", "-f", "-l", label, device_path]
    raise ValueError(f"unsupported fstype: {fstype}")


def _label_for(role: Role, device: OptaneDevice) -> str:
    base = {
        "slow_ai_memory": "zeos-aimem",
        "masq_temporal": "zeos-masq",
        "vault_hot": "zeos-vault",
        "swap_extension": "zeos-swap",
        "custom": "zeos-custom",
    }.get(role.key, "zeos-optane")
    # ext4 label is 16 chars; XFS is 12; keep it short.
    return base[:12]


def _resolve_mount_options(role: Role, device: OptaneDevice, fstype: str) -> str:
    opts = role.mount_options or "defaults"
    if device.kind == "pmem-fsdax" and fstype in ("ext4", "xfs") and "dax" not in opts:
        opts = f"{opts},dax"
    return opts


def build_plan(
    device: OptaneDevice,
    role: Role,
    *,
    mountpoint: Optional[str] = None,
    fstype: Optional[str] = None,
    force_format: bool = False,
) -> Plan:
    """Construct a Plan for materializing ``role`` on ``device``."""
    chosen_mp = mountpoint or role.default_mountpoint
    chosen_fs = fstype or role.default_fstype
    chosen_fs = chosen_fs if role.needs_filesystem else ""
    plan = Plan(
        device=device,
        role=role,
        mountpoint=chosen_mp,
        fstype=chosen_fs,
        mount_options=_resolve_mount_options(role, device, chosen_fs),
    )

    if role.key == "skip":
        plan.notes.append("Device left unchanged. Recorded in Zeos config only.")
        return plan

    # Refuse to operate on a device that is currently mounted (system or data).
    if device.mountpoint:
        plan.notes.append(
            f"Device is currently mounted at {device.mountpoint}. "
            f"Unmount manually before configuring."
        )
        return plan

    # ------------------------------------------------------------------
    # Swap role
    # ------------------------------------------------------------------
    if role.is_swap:
        if device.has_existing_filesystem() and not force_format:
            plan.notes.append(
                f"Device already carries '{device.fstype}'. Pass --force-format "
                f"to overwrite as swap."
            )
            return plan
        plan.steps.append(Step(["mkswap", "-L", _label_for(role, device), device.path],
                               f"Create swap signature on {device.path}", destructive=True))
        plan.steps.append(Step(["swapon", "--priority", "100", device.path],
                               f"Activate {device.path} as priority-100 swap"))
        plan.fstab_line = (
            f"LABEL={_label_for(role, device)}  none  swap  pri=100  0  0"
        )
        return plan

    # ------------------------------------------------------------------
    # Devdax: no filesystem, just record
    # ------------------------------------------------------------------
    if device.kind == "pmem-devdax":
        plan.notes.append(
            "Device is in devdax mode — no filesystem will be created. "
            "Zeos records this as a raw memory-mapped signal-graph endpoint."
        )
        return plan

    # ------------------------------------------------------------------
    # Filesystem-backed roles (slow_ai_memory, masq_temporal, vault_hot, custom)
    # ------------------------------------------------------------------
    if not chosen_mp:
        plan.notes.append(
            "No mountpoint chosen. Pass --mountpoint or pick a non-custom role."
        )
        return plan

    if device.has_existing_filesystem() and not force_format:
        plan.notes.append(
            f"Existing {device.fstype} filesystem detected on {device.path}. "
            f"Will mount as-is; pass --force-format to wipe and reformat."
        )
        plan.steps.append(Step(["mkdir", "-p", chosen_mp],
                               f"Ensure mountpoint {chosen_mp} exists"))
        plan.steps.append(Step(
            ["mount", "-t", device.fstype, "-o", plan.mount_options, device.path, chosen_mp],
            f"Mount existing {device.fstype} on {chosen_mp}",
        ))
        plan.fstab_line = _fstab_line(
            device, chosen_mp, device.fstype, plan.mount_options, role,
        )
        return plan

    # Fresh format
    plan.steps.append(Step(_mkfs_args(chosen_fs, device.path, _label_for(role, device)),
                           f"Create {chosen_fs} filesystem on {device.path}",
                           destructive=True))
    plan.steps.append(Step(["mkdir", "-p", chosen_mp],
                           f"Ensure mountpoint {chosen_mp} exists"))
    plan.steps.append(Step(
        ["mount", "-t", chosen_fs, "-o", plan.mount_options, device.path, chosen_mp],
        f"Mount {chosen_fs} on {chosen_mp}",
    ))
    plan.fstab_line = _fstab_line(device, chosen_mp, chosen_fs, plan.mount_options, role)
    return plan


def _fstab_line(
    device: OptaneDevice,
    mountpoint: str,
    fstype: str,
    options: str,
    role: Role,
) -> str:
    """Produce a stable fstab entry. Prefer LABEL= identifiers since UUID is
    unknown until after mkfs; the label we set is deterministic."""
    label = _label_for(role, device)
    return (
        f"LABEL={label}  {mountpoint}  {fstype}  {options}  "
        f"{role.fstab_dump}  {role.fstab_pass}"
    )


def render_plan(plan: Plan) -> str:
    """Pretty-print a plan for user review."""
    lines = [
        f"Device:     {plan.device.path}  ({plan.device.model})",
        f"Role:       {plan.role.key}  ({plan.role.label})",
    ]
    if plan.mountpoint:
        lines.append(f"Mountpoint: {plan.mountpoint}")
    if plan.fstype:
        lines.append(f"Filesystem: {plan.fstype}")
    if plan.mount_options:
        lines.append(f"Options:    {plan.mount_options}")
    if plan.steps:
        lines.append("Steps:")
        for i, step in enumerate(plan.steps, 1):
            marker = "!" if step.destructive else "-"
            lines.append(f"  {marker} [{i}] {step.description}")
            lines.append(f"        $ {' '.join(step.cmd)}")
    if plan.fstab_line:
        lines.append(f"fstab:     {plan.fstab_line}")
    if plan.notes:
        lines.append("Notes:")
        for note in plan.notes:
            lines.append(f"  * {note}")
    if plan.is_noop():
        lines.append("(plan is empty — nothing to apply)")
    return "\n".join(lines)
