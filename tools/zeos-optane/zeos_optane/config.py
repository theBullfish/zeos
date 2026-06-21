"""Persist the user's Optane configuration in a Zeos-native, forward-compatible
JSON file at ``/etc/zeos/optane.json``.

The schema below is the schema Zeos native consumes when it boots: every field
written here is a field the kernel signal-graph builder reads. Linux today, Zeos
tomorrow — same file, same path, no migration.

Schema (version 1):
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
          "serial": "...",
          "wwid": "...",
          "zeos_role": "slow_ai_memory",
          "signal_contract": "optane.slow_ai_memory.v1",
          "mountpoint": "/var/lib/zeos/ai-memory",
          "fstype": "ext4",
          "mount_options": "defaults,noatime,lazytime,dax",
          "fstab_label": "zeos-aimem",
          "applied_at": "<iso8601>",
          "applied_by": "zeos-optane <version>"
        },
        ...
      ]
    }
"""
from __future__ import annotations

import datetime as _dt
import json
import os
import socket
from dataclasses import dataclass, field, asdict
from pathlib import Path

from . import __version__
from .detect import OptaneDevice
from .plan import Plan
from .roles import Role


CONFIG_PATH = Path(os.environ.get("ZEOS_OPTANE_CONFIG", "/etc/zeos/optane.json"))
SPECS_DIR = Path(os.environ.get("ZEOS_OPTANE_SPECS_DIR", "/etc/zeos/specs"))
SCHEMA_VERSION = 1


@dataclass
class DeviceRecord:
    path: str
    kind: str
    model: str
    size_bytes: int
    serial: str
    wwid: str
    zeos_role: str
    signal_contract: str
    mountpoint: str
    fstype: str
    mount_options: str
    fstab_label: str
    applied_at: str
    applied_by: str

    def identity(self) -> str:
        """Stable identifier across reboots: prefer wwid, fall back to serial,
        then to the device path. fstab uses LABEL= so this is for in-config
        lookup only."""
        return self.wwid or self.serial or self.path


def _now_iso() -> str:
    return _dt.datetime.now(_dt.timezone.utc).isoformat(timespec="seconds")


def load() -> dict:
    """Read the config file. Return a fresh skeleton if it doesn't exist."""
    if not CONFIG_PATH.exists():
        return {
            "version": SCHEMA_VERSION,
            "host": socket.gethostname(),
            "updated_at": _now_iso(),
            "devices": [],
        }
    raw = CONFIG_PATH.read_text()
    if not raw.strip():
        return {"version": SCHEMA_VERSION, "host": socket.gethostname(),
                "updated_at": _now_iso(), "devices": []}
    data = json.loads(raw)
    if data.get("version") != SCHEMA_VERSION:
        raise RuntimeError(
            f"unsupported config schema version: {data.get('version')!r} "
            f"(expected {SCHEMA_VERSION})"
        )
    return data


def save(cfg: dict) -> None:
    cfg["updated_at"] = _now_iso()
    CONFIG_PATH.parent.mkdir(parents=True, exist_ok=True)
    tmp = CONFIG_PATH.with_suffix(".json.tmp")
    tmp.write_text(json.dumps(cfg, indent=2, sort_keys=True) + "\n")
    os.replace(tmp, CONFIG_PATH)
    try:
        os.chmod(CONFIG_PATH, 0o644)
    except PermissionError:
        pass


def record_from_plan(plan: Plan) -> DeviceRecord:
    label = ""
    # The fstab line we generated starts with "LABEL=<label>  ..."; pull it back out.
    if plan.fstab_line.startswith("LABEL="):
        label = plan.fstab_line.split()[0].split("=", 1)[1]
    return DeviceRecord(
        path=plan.device.path,
        kind=plan.device.kind,
        model=plan.device.model,
        size_bytes=plan.device.size_bytes,
        serial=plan.device.serial,
        wwid=plan.device.wwid,
        zeos_role=plan.role.key,
        signal_contract=plan.role.signal_contract,
        mountpoint=plan.mountpoint,
        fstype=plan.fstype,
        mount_options=plan.mount_options,
        fstab_label=label,
        applied_at=_now_iso(),
        applied_by=f"zeos-optane {__version__}",
    )


def upsert(cfg: dict, record: DeviceRecord) -> None:
    devices = cfg.setdefault("devices", [])
    rid = record.identity()
    for i, existing in enumerate(devices):
        existing_id = existing.get("wwid") or existing.get("serial") or existing.get("path")
        if existing_id == rid:
            devices[i] = asdict(record)
            return
    devices.append(asdict(record))


def remove(cfg: dict, identity: str) -> bool:
    devices = cfg.get("devices", [])
    for i, existing in enumerate(devices):
        existing_id = existing.get("wwid") or existing.get("serial") or existing.get("path")
        if existing_id == identity or existing.get("path") == identity:
            devices.pop(i)
            return True
    return False


def write_signal_contract(record: DeviceRecord) -> Path | None:
    """Emit a per-device signal contract under /etc/zeos/specs/.

    Zeos native reads this directory at boot to populate signal-graph nodes.
    Returns the path written, or None if the role is 'skip' (no contract)."""
    if not record.signal_contract:
        return None
    SPECS_DIR.mkdir(parents=True, exist_ok=True)
    safe_id = (record.wwid or record.serial or record.path.replace("/", "_")).replace("/", "_")
    target = SPECS_DIR / f"optane-{safe_id}.json"
    contract = {
        "contract": record.signal_contract,
        "device": {
            "path": record.path,
            "kind": record.kind,
            "model": record.model,
            "size_bytes": record.size_bytes,
            "serial": record.serial,
            "wwid": record.wwid,
        },
        "role": record.zeos_role,
        "mount": {
            "mountpoint": record.mountpoint,
            "fstype": record.fstype,
            "options": record.mount_options,
            "fstab_label": record.fstab_label,
        } if record.mountpoint else None,
        "applied_at": record.applied_at,
        "applied_by": record.applied_by,
        "schema_version": SCHEMA_VERSION,
    }
    tmp = target.with_suffix(".json.tmp")
    tmp.write_text(json.dumps(contract, indent=2, sort_keys=True) + "\n")
    os.replace(tmp, target)
    return target


def remove_signal_contract(record_or_identity) -> bool:
    """Delete the signal contract file for a given record or identity string."""
    if isinstance(record_or_identity, DeviceRecord):
        identity = record_or_identity.wwid or record_or_identity.serial or record_or_identity.path
    else:
        identity = record_or_identity
    safe_id = identity.replace("/", "_")
    target = SPECS_DIR / f"optane-{safe_id}.json"
    if target.exists():
        target.unlink()
        return True
    return False


def known_identities(cfg: dict) -> set[str]:
    """All device identities currently recorded in config."""
    out: set[str] = set()
    for d in cfg.get("devices", []):
        ident = d.get("wwid") or d.get("serial") or d.get("path")
        if ident:
            out.add(ident)
    return out


def device_identity(dev: OptaneDevice) -> str:
    return dev.wwid or dev.serial or dev.path
