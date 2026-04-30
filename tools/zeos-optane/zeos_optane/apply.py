"""Execute an apply Plan against the live system.

Safety rails (in order of priority):

  1. Dry-run by default. Real execution requires ``--yes``.
  2. Destructive steps (mkfs, mkswap) require an additional typed-device
     confirmation: the user types the device path back to proceed. This makes
     it physically impossible to format the wrong disk by tab-complete or by
     muscle memory.
  3. /etc/fstab is backed up to /etc/fstab.zeos-optane.bak.<timestamp> before
     any append. Appends are idempotent — a line for the same device+mountpoint
     is not written twice.
  4. Root is required for any step that touches device state. Detection and
     planning work fine as a normal user.
"""
from __future__ import annotations

import datetime as _dt
import os
import shutil
import subprocess
import sys
from pathlib import Path

from . import config as cfg_mod
from .plan import Plan, Step

FSTAB = Path(os.environ.get("ZEOS_OPTANE_FSTAB", "/etc/fstab"))


class ApplyError(RuntimeError):
    pass


def _is_root() -> bool:
    return os.geteuid() == 0


def _confirm_destructive(step: Step, device_path: str, *, assume_yes: bool) -> bool:
    """Even with --yes, destructive steps require a typed device confirmation
    unless the caller passes ``ZEOS_OPTANE_NO_TYPED_CONFIRM=1`` (used in tests)."""
    if os.environ.get("ZEOS_OPTANE_NO_TYPED_CONFIRM") == "1":
        return assume_yes
    print(f"\n!!  DESTRUCTIVE STEP  !!")
    print(f"    {step.description}")
    print(f"    $ {' '.join(step.cmd)}")
    print(f"\n    To confirm, type the device path exactly: {device_path}")
    try:
        typed = input("    > ").strip()
    except EOFError:
        return False
    if typed != device_path:
        print(f"    Mismatch ({typed!r} != {device_path!r}); aborting this step.")
        return False
    return True


def _run_step(step: Step, *, dry_run: bool) -> None:
    print(f"  $ {' '.join(step.cmd)}")
    if dry_run:
        return
    proc = subprocess.run(step.cmd, capture_output=True, text=True)
    if proc.stdout.strip():
        for line in proc.stdout.rstrip().splitlines():
            print(f"      {line}")
    if proc.returncode != 0:
        for line in (proc.stderr or "").rstrip().splitlines():
            print(f"      {line}", file=sys.stderr)
        raise ApplyError(
            f"step failed (rc={proc.returncode}): {' '.join(step.cmd)}"
        )


def _backup_fstab() -> Path:
    ts = _dt.datetime.now().strftime("%Y%m%dT%H%M%S")
    target = FSTAB.with_name(f"fstab.zeos-optane.bak.{ts}")
    shutil.copy2(FSTAB, target)
    return target


def _fstab_contains(line: str) -> bool:
    if not FSTAB.exists():
        return False
    needle = line.strip().split()
    # Match on (source, mountpoint) pair to dedupe regardless of options drift.
    if len(needle) < 2:
        return False
    src, mp = needle[0], needle[1]
    for raw in FSTAB.read_text().splitlines():
        s = raw.strip()
        if not s or s.startswith("#"):
            continue
        parts = s.split()
        if len(parts) >= 2 and parts[0] == src and parts[1] == mp:
            return True
    return False


def _append_fstab(line: str, *, dry_run: bool) -> None:
    print(f"  fstab: {line}")
    if dry_run:
        return
    if _fstab_contains(line):
        print("      (already present in /etc/fstab — skipping)")
        return
    backup = _backup_fstab()
    print(f"      backed up /etc/fstab -> {backup}")
    with FSTAB.open("a") as fh:
        if FSTAB.read_text() and not FSTAB.read_text().endswith("\n"):
            fh.write("\n")
        fh.write(f"# zeos-optane: {line.split()[0]}\n")
        fh.write(line + "\n")


def apply_plan(
    plan: Plan,
    *,
    dry_run: bool = True,
    assume_yes: bool = False,
) -> None:
    """Execute the steps in ``plan`` and update the persistent Zeos config."""
    if plan.is_noop():
        print("Nothing to apply.")
        for note in plan.notes:
            print(f"  * {note}")
        # Skip role still gets recorded so 'status' reports it.
        if plan.role.key == "skip":
            _record(plan, dry_run=dry_run)
        return

    if not dry_run and not _is_root():
        raise ApplyError("apply requires root (rerun with sudo, or pass --dry-run)")

    print(f"\nApplying plan for {plan.device.path} ({plan.role.key}):")
    if dry_run:
        print("  (dry-run — no changes will be made)")

    for step in plan.steps:
        if step.destructive:
            if not assume_yes:
                raise ApplyError(
                    "destructive step encountered; pass --yes to proceed"
                )
            if not dry_run and not _confirm_destructive(
                step, plan.device.path, assume_yes=assume_yes
            ):
                raise ApplyError("destructive step not confirmed; aborting")
        _run_step(step, dry_run=dry_run)

    if plan.fstab_line:
        _append_fstab(plan.fstab_line, dry_run=dry_run)

    _record(plan, dry_run=dry_run)
    print("Done.")
    for note in plan.notes:
        print(f"  * {note}")


def _record(plan: Plan, *, dry_run: bool) -> None:
    """Update /etc/zeos/optane.json and emit the per-device signal contract."""
    record = cfg_mod.record_from_plan(plan)
    if dry_run:
        print(f"  config: would record {record.path} as {record.zeos_role}")
        if record.signal_contract:
            print(f"  spec:   would write {cfg_mod.SPECS_DIR}/optane-*.json")
        return
    cfg = cfg_mod.load()
    cfg_mod.upsert(cfg, record)
    cfg_mod.save(cfg)
    print(f"  config: recorded {record.path} as {record.zeos_role} in {cfg_mod.CONFIG_PATH}")
    spec_path = cfg_mod.write_signal_contract(record)
    if spec_path:
        print(f"  spec:   wrote {spec_path}")


def unconfigure(identity: str, *, dry_run: bool = True) -> None:
    """Remove a device from the Zeos config and its signal contract.

    This intentionally does NOT unmount the device or rewrite /etc/fstab —
    those operations are irreversible enough that we want the user to do them
    by hand. We just remove the Zeos-side bookkeeping; the printed message
    tells the user the manual cleanup steps."""
    cfg = cfg_mod.load()
    devices = cfg.get("devices", [])
    target = None
    for d in devices:
        ident = d.get("wwid") or d.get("serial") or d.get("path")
        if ident == identity or d.get("path") == identity:
            target = d
            break
    if target is None:
        print(f"No record found for '{identity}'.")
        return
    print(f"Removing {target['path']} ({target['zeos_role']}) from Zeos config.")
    if dry_run:
        print("  (dry-run — no changes will be made)")
        return
    cfg_mod.remove(cfg, identity)
    cfg_mod.save(cfg)
    cfg_mod.remove_signal_contract(target.get("wwid") or target.get("serial") or target.get("path"))
    print("Manual cleanup (not performed automatically):")
    if target.get("mountpoint"):
        print(f"  sudo umount {target['mountpoint']}")
        print(f"  # then remove the corresponding entry from /etc/fstab")
    elif target.get("zeos_role") == "swap_extension":
        print(f"  sudo swapoff {target['path']}")
        print(f"  # then remove the corresponding entry from /etc/fstab")
