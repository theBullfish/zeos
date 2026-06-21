"""Detect Intel Optane devices on the host.

Two device classes are supported:

  * NVMe Optane SSDs (P-series, M-series, H-series hybrid) — block devices
    addressed as ``/dev/nvmeXnY``. Identified by NVMe controller model strings.
  * Optane Persistent Memory DIMMs (Apache Pass / Barlow Pass) exposed via
    libnvdimm as ``/dev/pmem*`` (fsdax / sector / raw) or ``/dev/dax*`` (devdax).

Detection is read-only and does not require root. ``lsblk`` is required (it ships
with util-linux and is present on every supported distro). ``ndctl`` is optional;
if absent, PMem DIMMs are still discovered via sysfs.
"""
from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
from dataclasses import dataclass, field, asdict
from pathlib import Path
from typing import Iterable

# Model-string fragments observed across Intel Optane product lines.
# Keep this list narrow — false positives here would offer to format the wrong drive.
OPTANE_MODEL_PATTERNS = [
    re.compile(r"\bOPTANE\b", re.IGNORECASE),
    re.compile(r"INTEL\s+SSDPED", re.IGNORECASE),     # DC P4800X / P4801X / P4810X / P1600X
    re.compile(r"INTEL\s+SSDPEK", re.IGNORECASE),     # 800P / M10 M.2
    re.compile(r"INTEL\s+SSDPEL", re.IGNORECASE),     # P5800X family
    re.compile(r"INTEL\s+SSDPF21Q", re.IGNORECASE),   # P5800X (alt enum)
    re.compile(r"INTEL\s+MEMPEK", re.IGNORECASE),     # Memory M.2 (16/32 GB cache)
    re.compile(r"INTEL\s+MEMPEL", re.IGNORECASE),
    re.compile(r"INTEL\s+HBRPEK", re.IGNORECASE),     # H10/H20 hybrid (Optane half)
]


@dataclass
class OptaneDevice:
    """A single Optane block or memory device."""

    path: str                       # e.g. /dev/nvme1n1, /dev/pmem0, /dev/dax0.0
    kind: str                       # "nvme" | "pmem-fsdax" | "pmem-sector" | "pmem-devdax" | "pmem-raw"
    model: str                      # human-readable model string
    size_bytes: int                 # 0 if unknown
    serial: str = ""
    wwid: str = ""
    uuid: str = ""                  # filesystem UUID if one already exists
    fstype: str = ""                # existing filesystem type, "" if blank
    mountpoint: str = ""            # current mountpoint, "" if not mounted
    label: str = ""
    extras: dict = field(default_factory=dict)

    def is_pmem(self) -> bool:
        return self.kind.startswith("pmem")

    def supports_dax(self) -> bool:
        return self.kind in ("pmem-fsdax", "pmem-devdax")

    def has_existing_filesystem(self) -> bool:
        return bool(self.fstype) and self.fstype not in ("swap",)

    def to_dict(self) -> dict:
        return asdict(self)


def _model_is_optane(model: str) -> bool:
    if not model:
        return False
    return any(p.search(model) for p in OPTANE_MODEL_PATTERNS)


def _human_size_to_bytes(value) -> int:
    """lsblk -b gives bytes as int or string; tolerate both."""
    if value is None or value == "":
        return 0
    try:
        return int(value)
    except (TypeError, ValueError):
        return 0


def _run(cmd: list[str], check: bool = False) -> tuple[int, str, str]:
    """Run a command, never raising on non-zero unless ``check`` is set."""
    try:
        proc = subprocess.run(
            cmd, capture_output=True, text=True, check=check, timeout=15
        )
    except FileNotFoundError:
        return 127, "", f"{cmd[0]}: not found"
    except subprocess.TimeoutExpired:
        return 124, "", f"{cmd[0]}: timed out"
    return proc.returncode, proc.stdout, proc.stderr


# ---------------------------------------------------------------------------
# NVMe detection
# ---------------------------------------------------------------------------

def _nvme_devices_from_lsblk(lsblk_json: dict) -> list[OptaneDevice]:
    """Walk lsblk output, returning Optane-matching NVMe devices."""
    out: list[OptaneDevice] = []
    for parent in lsblk_json.get("blockdevices", []):
        if (parent.get("tran") or "").lower() != "nvme":
            continue
        model = (parent.get("model") or "").strip()
        if not _model_is_optane(model):
            continue
        # Each parent (the controller's namespace) is itself the block device
        # we want to act on. lsblk -d returns one row per disk already.
        out.append(
            OptaneDevice(
                path=f"/dev/{parent['name']}",
                kind="nvme",
                model=model,
                size_bytes=_human_size_to_bytes(parent.get("size")),
                serial=(parent.get("serial") or "").strip(),
                wwid=(parent.get("wwn") or "").strip(),
                uuid=(parent.get("uuid") or "").strip(),
                fstype=(parent.get("fstype") or "").strip(),
                mountpoint=(parent.get("mountpoint") or "").strip(),
                label=(parent.get("label") or "").strip(),
            )
        )
    return out


def _read_lsblk(fixture: str | None = None) -> dict:
    if fixture:
        return json.loads(Path(fixture).read_text())
    rc, stdout, stderr = _run([
        "lsblk", "-d", "-b", "-J",
        "-o", "NAME,MODEL,SERIAL,SIZE,TRAN,FSTYPE,MOUNTPOINT,UUID,LABEL,WWN",
    ])
    if rc != 0 or not stdout.strip():
        return {"blockdevices": []}
    try:
        return json.loads(stdout)
    except json.JSONDecodeError:
        return {"blockdevices": []}


# ---------------------------------------------------------------------------
# PMem detection
# ---------------------------------------------------------------------------

def _read_ndctl(fixture: str | None = None) -> list[dict]:
    """Return ndctl namespace records, or [] if ndctl is unavailable."""
    if fixture:
        return json.loads(Path(fixture).read_text())
    if shutil.which("ndctl") is None:
        return []
    rc, stdout, _ = _run(["ndctl", "list", "-N", "-u"])
    if rc != 0 or not stdout.strip():
        return []
    try:
        data = json.loads(stdout)
    except json.JSONDecodeError:
        return []
    if isinstance(data, dict):  # single namespace ndctl returns an object
        return [data]
    return data


def _pmem_kind(mode: str) -> str:
    return {
        "fsdax": "pmem-fsdax",
        "devdax": "pmem-devdax",
        "sector": "pmem-sector",
        "raw": "pmem-raw",
    }.get((mode or "").lower(), "pmem-raw")


def _pmem_path(ns: dict) -> str:
    if ns.get("daxregion") and ns.get("chardev"):
        return f"/dev/{ns['chardev']}"
    if ns.get("blockdev"):
        return f"/dev/{ns['blockdev']}"
    if ns.get("chardev"):
        return f"/dev/{ns['chardev']}"
    return ""


def _pmem_devices_from_ndctl(records: list[dict]) -> list[OptaneDevice]:
    out: list[OptaneDevice] = []
    for ns in records:
        path = _pmem_path(ns)
        if not path:
            continue
        size = ns.get("size")
        out.append(
            OptaneDevice(
                path=path,
                kind=_pmem_kind(ns.get("mode", "")),
                model="Intel Persistent Memory (Optane PMem)",
                size_bytes=_human_size_to_bytes(size),
                serial=ns.get("uuid", ""),
                wwid=ns.get("uuid", ""),
                extras={
                    "namespace": ns.get("dev", ""),
                    "mode": ns.get("mode", ""),
                    "region": ns.get("region", ""),
                },
            )
        )
    return out


def _pmem_devices_from_sysfs() -> list[OptaneDevice]:
    """Fallback when ndctl is missing: walk /sys/class/block for pmem*."""
    out: list[OptaneDevice] = []
    sysblock = Path("/sys/class/block")
    if not sysblock.is_dir():
        return out
    for entry in sorted(sysblock.iterdir()):
        if not entry.name.startswith("pmem"):
            continue
        size_sectors = 0
        try:
            size_sectors = int((entry / "size").read_text().strip())
        except (OSError, ValueError):
            pass
        out.append(
            OptaneDevice(
                path=f"/dev/{entry.name}",
                kind="pmem-fsdax",  # most common default; user can override
                model="Intel Persistent Memory (Optane PMem)",
                size_bytes=size_sectors * 512,
                extras={"detected_via": "sysfs"},
            )
        )
    return out


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def detect_optane_devices(
    lsblk_fixture: str | None = None,
    ndctl_fixture: str | None = None,
) -> list[OptaneDevice]:
    """Return all Optane devices visible on this host."""
    nvme = _nvme_devices_from_lsblk(_read_lsblk(lsblk_fixture))
    ndctl_records = _read_ndctl(ndctl_fixture)
    pmem = _pmem_devices_from_ndctl(ndctl_records)
    if not pmem and not ndctl_fixture:
        pmem = _pmem_devices_from_sysfs()
    return nvme + pmem


def detect_summary(devices: Iterable[OptaneDevice]) -> str:
    """Render a human-readable summary table."""
    devices = list(devices)
    if not devices:
        return "No Optane devices detected."
    lines = []
    for i, d in enumerate(devices, 1):
        size_gib = d.size_bytes / (1024 ** 3) if d.size_bytes else 0
        used = f"  [in use: {d.fstype}@{d.mountpoint}]" if d.mountpoint else ""
        lines.append(
            f"  [{i}] {d.path}  {d.model}  ({size_gib:.1f} GiB, {d.kind}){used}"
        )
    return "\n".join(lines)
