"""``zeos-optane`` CLI.

Subcommands:

  scan          List Optane devices on this host (read-only).
  configure     Interactive: prompt per device, build a plan, optionally apply.
  apply         Non-interactive: --device DEV --role ROLE [--mountpoint PATH].
  status        Show currently-configured Optane devices.
  unconfigure   Remove a device from the Zeos config (manual unmount).
  check         Boot-time sanity check; prints unconfigured devices to stderr.

Global flags:
  --json        Machine-readable output for scan / status / check.
  --dry-run     Plan-only; never modifies system state. Default for safety.
  --yes         Permit destructive steps. Required for any apply operation.
"""
from __future__ import annotations

import argparse
import json
import sys
from typing import Optional

from . import __version__
from . import apply as apply_mod
from . import config as cfg_mod
from .detect import detect_optane_devices, detect_summary, OptaneDevice
from .plan import build_plan, render_plan
from .roles import ROLES, role_choices, role_for_index, role_for_key


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _devices_or_exit(lsblk_fixture: Optional[str], ndctl_fixture: Optional[str]) -> list[OptaneDevice]:
    devs = detect_optane_devices(lsblk_fixture=lsblk_fixture, ndctl_fixture=ndctl_fixture)
    return devs


def _find_device(devices: list[OptaneDevice], spec: str) -> OptaneDevice:
    for d in devices:
        if d.path == spec or d.wwid == spec or d.serial == spec:
            return d
    raise SystemExit(f"error: no Optane device matches '{spec}'")


def _prompt(prompt: str, default: str = "") -> str:
    """Read a line from stdin with a default fallback."""
    suffix = f" [{default}]" if default else ""
    try:
        ans = input(f"{prompt}{suffix}: ").strip()
    except EOFError:
        return default
    return ans or default


# ---------------------------------------------------------------------------
# scan
# ---------------------------------------------------------------------------

def cmd_scan(args: argparse.Namespace) -> int:
    devs = _devices_or_exit(args.lsblk_fixture, args.ndctl_fixture)
    if args.json:
        print(json.dumps([d.to_dict() for d in devs], indent=2))
        return 0
    if not devs:
        print("No Optane devices detected.")
        return 0
    print(f"Detected {len(devs)} Optane device(s):")
    print(detect_summary(devs))
    return 0


# ---------------------------------------------------------------------------
# configure (interactive)
# ---------------------------------------------------------------------------

def cmd_configure(args: argparse.Namespace) -> int:
    devs = _devices_or_exit(args.lsblk_fixture, args.ndctl_fixture)
    if not devs:
        print("No Optane devices detected.")
        return 0
    print(f"Detected {len(devs)} Optane device(s).\n")
    cfg = cfg_mod.load()
    known = cfg_mod.known_identities(cfg)

    for i, dev in enumerate(devs, 1):
        ident = cfg_mod.device_identity(dev)
        already = " (already configured)" if ident in known else ""
        print(f"--- Device [{i}/{len(devs)}]: {dev.path} ---{already}")
        print(f"  model: {dev.model}")
        print(f"  size:  {dev.size_bytes / (1024**3):.1f} GiB")
        print(f"  kind:  {dev.kind}")
        if dev.fstype:
            print(f"  existing filesystem: {dev.fstype} (mount: {dev.mountpoint or '-'})")
        print("\nWhat should this device be used for?")
        print(role_choices())
        choice = _prompt("\n  Choice", "1")
        try:
            role = role_for_index(int(choice))
        except (ValueError, TypeError):
            print("  invalid choice; skipping device.")
            continue

        mp = role.default_mountpoint
        fs = role.default_fstype
        if role.key == "custom":
            mp = _prompt("  Mountpoint", "/var/lib/zeos/optane")
            fs = _prompt("  Filesystem (ext4/xfs/f2fs)", "ext4")

        plan = build_plan(dev, role, mountpoint=mp, fstype=fs,
                          force_format=args.force_format)
        print()
        print(render_plan(plan))
        if plan.is_noop():
            continue
        if args.dry_run:
            print("\n(dry-run — not applying)")
            continue
        ans = _prompt("\nApply this plan? (yes/no)", "no").lower()
        if ans not in ("y", "yes"):
            print("  skipped.")
            continue
        try:
            apply_mod.apply_plan(plan, dry_run=False, assume_yes=True)
        except apply_mod.ApplyError as exc:
            print(f"  error: {exc}", file=sys.stderr)

    return 0


# ---------------------------------------------------------------------------
# apply (non-interactive)
# ---------------------------------------------------------------------------

def cmd_apply(args: argparse.Namespace) -> int:
    devs = _devices_or_exit(args.lsblk_fixture, args.ndctl_fixture)
    dev = _find_device(devs, args.device)
    role = role_for_key(args.role)
    plan = build_plan(
        dev, role,
        mountpoint=args.mountpoint,
        fstype=args.fstype,
        force_format=args.force_format,
    )
    print(render_plan(plan))
    if plan.is_noop():
        return 0
    try:
        apply_mod.apply_plan(plan, dry_run=args.dry_run, assume_yes=args.yes)
    except apply_mod.ApplyError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    return 0


# ---------------------------------------------------------------------------
# status
# ---------------------------------------------------------------------------

def cmd_status(args: argparse.Namespace) -> int:
    cfg = cfg_mod.load()
    if args.json:
        print(json.dumps(cfg, indent=2))
        return 0
    devices = cfg.get("devices", [])
    print(f"Zeos Optane config: {cfg_mod.CONFIG_PATH}")
    print(f"  schema version: {cfg.get('version')}")
    print(f"  host:           {cfg.get('host')}")
    print(f"  updated_at:     {cfg.get('updated_at')}")
    if not devices:
        print("  (no devices recorded)")
        return 0
    print(f"  devices ({len(devices)}):")
    for d in devices:
        print(f"    - {d['path']}  role={d['zeos_role']}  mount={d.get('mountpoint') or '-'}  "
              f"fstype={d.get('fstype') or '-'}  applied={d.get('applied_at')}")
    return 0


# ---------------------------------------------------------------------------
# unconfigure
# ---------------------------------------------------------------------------

def cmd_unconfigure(args: argparse.Namespace) -> int:
    apply_mod.unconfigure(args.device, dry_run=args.dry_run)
    return 0


# ---------------------------------------------------------------------------
# check (boot-time / udev hook)
# ---------------------------------------------------------------------------

def cmd_check(args: argparse.Namespace) -> int:
    """Compare detected devices against /etc/zeos/optane.json. Print a
    notification for any device not yet recorded. Exit 0 always — this is
    advisory, not a failure path."""
    cfg = cfg_mod.load()
    known = cfg_mod.known_identities(cfg)
    devs = detect_optane_devices(
        lsblk_fixture=args.lsblk_fixture, ndctl_fixture=args.ndctl_fixture,
    )
    new = [d for d in devs if cfg_mod.device_identity(d) not in known]
    if args.json:
        print(json.dumps({
            "detected": [d.to_dict() for d in devs],
            "unconfigured": [d.to_dict() for d in new],
        }, indent=2))
        return 0
    if not new:
        print("All detected Optane devices are configured.")
        return 0
    print(f"Found {len(new)} unconfigured Optane device(s):", file=sys.stderr)
    print(detect_summary(new), file=sys.stderr)
    print("\nRun  sudo zeos-optane configure  to integrate them.", file=sys.stderr)
    return 0


# ---------------------------------------------------------------------------
# Argparse plumbing
# ---------------------------------------------------------------------------

def _add_fixture_args(p: argparse.ArgumentParser) -> None:
    p.add_argument("--lsblk-fixture", help=argparse.SUPPRESS)
    p.add_argument("--ndctl-fixture", help=argparse.SUPPRESS)


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="zeos-optane",
        description=(
            "Detect Intel Optane devices and integrate them into the Zeos "
            "memory tier. Runs on stock Linux today; persistent state is "
            "Zeos-native so the configuration carries forward when Zeos "
            "boots on the same machine."
        ),
    )
    p.add_argument("--version", action="version", version=f"zeos-optane {__version__}")
    sub = p.add_subparsers(dest="cmd", required=True)

    sp = sub.add_parser("scan", help="list Optane devices visible on this host")
    sp.add_argument("--json", action="store_true")
    _add_fixture_args(sp)
    sp.set_defaults(func=cmd_scan)

    sp = sub.add_parser("configure", help="interactive: prompt per device and apply")
    sp.add_argument("--dry-run", action="store_true",
                    help="plan only; never modify system state")
    sp.add_argument("--force-format", action="store_true",
                    help="permit reformatting of devices that already carry a filesystem")
    _add_fixture_args(sp)
    sp.set_defaults(func=cmd_configure)

    sp = sub.add_parser("apply", help="non-interactive: configure a single device")
    sp.add_argument("--device", required=True, help="device path, wwid, or serial")
    sp.add_argument("--role", required=True, choices=[r.key for r in ROLES],
                    help="Zeos role for this device")
    sp.add_argument("--mountpoint", help="override default mountpoint")
    sp.add_argument("--fstype", help="override default filesystem (ext4/xfs/f2fs)")
    sp.add_argument("--dry-run", action="store_true", default=True,
                    help="(default) plan only")
    sp.add_argument("--no-dry-run", dest="dry_run", action="store_false",
                    help="actually execute the plan (requires --yes)")
    sp.add_argument("--yes", action="store_true",
                    help="permit destructive steps (mkfs, mkswap)")
    sp.add_argument("--force-format", action="store_true")
    _add_fixture_args(sp)
    sp.set_defaults(func=cmd_apply)

    sp = sub.add_parser("status", help="show currently-configured Optane devices")
    sp.add_argument("--json", action="store_true")
    sp.set_defaults(func=cmd_status)

    sp = sub.add_parser("unconfigure", help="remove a device from the Zeos config")
    sp.add_argument("--device", required=True, help="device path, wwid, or serial")
    sp.add_argument("--dry-run", action="store_true")
    sp.set_defaults(func=cmd_unconfigure)

    sp = sub.add_parser("check", help="compare detected vs configured (boot/udev hook)")
    sp.add_argument("--json", action="store_true")
    _add_fixture_args(sp)
    sp.set_defaults(func=cmd_check)

    return p


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
