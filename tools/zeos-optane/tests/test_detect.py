"""Unit tests for zeos_optane.

These tests use JSON fixtures in tests/fixtures/ to exercise the detection,
role, plan, and config logic without touching real devices. Run with:

    cd tools/zeos-optane
    python3 -m unittest discover -s tests -v
"""
from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

# Make the package importable when running tests from the tool directory.
ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from zeos_optane import detect, plan as plan_mod, roles, config as cfg_mod  # noqa: E402

FIXTURES = Path(__file__).parent / "fixtures"


class DetectTests(unittest.TestCase):
    def test_detects_nvme_optane_only(self):
        devs = detect.detect_optane_devices(
            lsblk_fixture=str(FIXTURES / "lsblk_optane_nvme.json"),
            ndctl_fixture=str(FIXTURES / "ndctl_empty.json"),
        )
        self.assertEqual(len(devs), 2)
        paths = {d.path for d in devs}
        self.assertEqual(paths, {"/dev/nvme1n1", "/dev/nvme2n1"})
        for d in devs:
            self.assertEqual(d.kind, "nvme")
        # Non-Optane NVMe and SATA drives must not match.
        for d in devs:
            self.assertNotIn("Samsung", d.model)
            self.assertNotIn("WDC", d.model)

    def test_no_match_when_no_optane(self):
        devs = detect.detect_optane_devices(
            lsblk_fixture=str(FIXTURES / "lsblk_no_optane.json"),
            ndctl_fixture=str(FIXTURES / "ndctl_empty.json"),
        )
        self.assertEqual(devs, [])

    def test_detects_pmem_fsdax_and_devdax(self):
        devs = detect.detect_optane_devices(
            lsblk_fixture=str(FIXTURES / "lsblk_no_optane.json"),
            ndctl_fixture=str(FIXTURES / "ndctl_pmem_fsdax.json"),
        )
        kinds = sorted(d.kind for d in devs)
        self.assertEqual(kinds, ["pmem-devdax", "pmem-fsdax"])
        fsdax = next(d for d in devs if d.kind == "pmem-fsdax")
        self.assertEqual(fsdax.path, "/dev/pmem0")
        self.assertTrue(fsdax.supports_dax())
        devdax = next(d for d in devs if d.kind == "pmem-devdax")
        self.assertEqual(devdax.path, "/dev/dax1.0")

    def test_combined_detection(self):
        devs = detect.detect_optane_devices(
            lsblk_fixture=str(FIXTURES / "lsblk_optane_nvme.json"),
            ndctl_fixture=str(FIXTURES / "ndctl_pmem_fsdax.json"),
        )
        self.assertEqual(len(devs), 4)  # 2 nvme + 2 pmem

    def test_model_pattern_specificity(self):
        # The detector must not flag non-Intel NVMe drives, even if they're
        # named with overlapping substrings.
        self.assertFalse(detect._model_is_optane("Samsung SSD 980 PRO 1TB"))
        self.assertFalse(detect._model_is_optane("WDC WDS500G2B0C"))
        self.assertFalse(detect._model_is_optane(""))
        self.assertTrue(detect._model_is_optane("INTEL OPTANE SSD 905P"))
        self.assertTrue(detect._model_is_optane("Intel SSDPED1K375GA"))
        self.assertTrue(detect._model_is_optane("INTEL MEMPEK1J032GA"))


class RoleTests(unittest.TestCase):
    def test_all_roles_have_unique_keys(self):
        keys = [r.key for r in roles.ROLES]
        self.assertEqual(len(keys), len(set(keys)))

    def test_role_lookup_by_index_and_key(self):
        ai = roles.role_for_index(1)
        self.assertEqual(ai.key, "slow_ai_memory")
        self.assertIs(ai, roles.role_for_key("slow_ai_memory"))

    def test_skip_role_is_noop_capable(self):
        skip = roles.role_for_key("skip")
        self.assertFalse(skip.needs_filesystem)
        self.assertFalse(skip.is_swap)
        self.assertEqual(skip.signal_contract, "")


class PlanTests(unittest.TestCase):
    def _device(self, **overrides):
        d = detect.OptaneDevice(
            path="/dev/nvme1n1",
            kind="nvme",
            model="INTEL SSDPED1K375GA",
            size_bytes=375083606016,
            serial="PHKE000000000",
            wwid="eui.01000000010000005CD2E4F1F4F40000",
        )
        for k, v in overrides.items():
            setattr(d, k, v)
        return d

    def test_slow_ai_memory_plan_on_blank_device(self):
        d = self._device()
        p = plan_mod.build_plan(d, roles.role_for_key("slow_ai_memory"))
        cmds = [s.cmd[0] for s in p.steps]
        self.assertEqual(cmds, ["mkfs.ext4", "mkdir", "mount"])
        self.assertTrue(p.steps[0].destructive)
        self.assertTrue(p.fstab_line.startswith("LABEL=zeos-aimem"))
        self.assertEqual(p.mountpoint, "/var/lib/zeos/ai-memory")

    def test_existing_filesystem_is_not_reformatted_by_default(self):
        d = self._device(fstype="ext4")
        p = plan_mod.build_plan(d, roles.role_for_key("slow_ai_memory"))
        cmds = [s.cmd[0] for s in p.steps]
        self.assertNotIn("mkfs.ext4", cmds)
        self.assertIn("mount", cmds)
        self.assertTrue(any("Existing ext4" in n for n in p.notes))

    def test_force_format_reformats(self):
        d = self._device(fstype="ext4")
        p = plan_mod.build_plan(d, roles.role_for_key("slow_ai_memory"),
                                force_format=True)
        cmds = [s.cmd[0] for s in p.steps]
        self.assertIn("mkfs.ext4", cmds)

    def test_swap_plan(self):
        d = self._device()
        p = plan_mod.build_plan(d, roles.role_for_key("swap_extension"))
        cmds = [s.cmd[0] for s in p.steps]
        self.assertEqual(cmds, ["mkswap", "swapon"])
        self.assertIn("pri=100", p.fstab_line)
        self.assertEqual(p.mountpoint, "")

    def test_skip_plan_is_noop(self):
        d = self._device()
        p = plan_mod.build_plan(d, roles.role_for_key("skip"))
        self.assertTrue(p.is_noop())

    def test_fsdax_pmem_gets_dax_mount_option(self):
        d = self._device(kind="pmem-fsdax", path="/dev/pmem0",
                         model="Intel Persistent Memory")
        p = plan_mod.build_plan(d, roles.role_for_key("masq_temporal"))
        self.assertIn("dax", p.mount_options)

    def test_devdax_pmem_makes_no_filesystem(self):
        d = self._device(kind="pmem-devdax", path="/dev/dax1.0",
                         model="Intel Persistent Memory")
        p = plan_mod.build_plan(d, roles.role_for_key("slow_ai_memory"))
        self.assertEqual(p.steps, [])
        self.assertTrue(any("devdax" in n for n in p.notes))

    def test_mounted_device_is_refused(self):
        d = self._device(fstype="ext4", mountpoint="/")
        p = plan_mod.build_plan(d, roles.role_for_key("slow_ai_memory"))
        self.assertEqual(p.steps, [])
        self.assertTrue(any("currently mounted" in n for n in p.notes))


class ConfigTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.cfg_path = Path(self.tmp.name) / "optane.json"
        self.specs_dir = Path(self.tmp.name) / "specs"
        # Patch module-level paths.
        self._orig_cfg = cfg_mod.CONFIG_PATH
        self._orig_specs = cfg_mod.SPECS_DIR
        cfg_mod.CONFIG_PATH = self.cfg_path
        cfg_mod.SPECS_DIR = self.specs_dir

    def tearDown(self):
        cfg_mod.CONFIG_PATH = self._orig_cfg
        cfg_mod.SPECS_DIR = self._orig_specs
        self.tmp.cleanup()

    def _record(self, **overrides):
        defaults = dict(
            path="/dev/nvme1n1",
            kind="nvme",
            model="INTEL SSDPED1K375GA",
            size_bytes=375083606016,
            serial="PHKE000000000",
            wwid="eui.01000000010000005CD2E4F1F4F40000",
            zeos_role="slow_ai_memory",
            signal_contract="optane.slow_ai_memory.v1",
            mountpoint="/var/lib/zeos/ai-memory",
            fstype="ext4",
            mount_options="defaults,noatime,lazytime",
            fstab_label="zeos-aimem",
            applied_at="2026-04-30T00:00:00+00:00",
            applied_by="zeos-optane 0.1.0",
        )
        defaults.update(overrides)
        return cfg_mod.DeviceRecord(**defaults)

    def test_load_returns_skeleton_when_missing(self):
        cfg = cfg_mod.load()
        self.assertEqual(cfg["version"], cfg_mod.SCHEMA_VERSION)
        self.assertEqual(cfg["devices"], [])

    def test_upsert_adds_then_replaces_by_wwid(self):
        cfg = cfg_mod.load()
        cfg_mod.upsert(cfg, self._record())
        cfg_mod.upsert(cfg, self._record(zeos_role="masq_temporal",
                                          mountpoint="/var/lib/zeos/masq",
                                          signal_contract="optane.masq_temporal.v1",
                                          fstab_label="zeos-masq"))
        self.assertEqual(len(cfg["devices"]), 1)
        self.assertEqual(cfg["devices"][0]["zeos_role"], "masq_temporal")

    def test_save_and_load_roundtrip(self):
        cfg = cfg_mod.load()
        cfg_mod.upsert(cfg, self._record())
        cfg_mod.save(cfg)
        loaded = cfg_mod.load()
        self.assertEqual(loaded["devices"][0]["wwid"],
                         "eui.01000000010000005CD2E4F1F4F40000")
        self.assertEqual(loaded["version"], cfg_mod.SCHEMA_VERSION)

    def test_signal_contract_is_written(self):
        rec = self._record()
        path = cfg_mod.write_signal_contract(rec)
        self.assertIsNotNone(path)
        self.assertTrue(path.exists())
        contract = json.loads(path.read_text())
        self.assertEqual(contract["contract"], "optane.slow_ai_memory.v1")
        self.assertEqual(contract["role"], "slow_ai_memory")
        self.assertEqual(contract["device"]["path"], "/dev/nvme1n1")
        self.assertEqual(contract["mount"]["fstab_label"], "zeos-aimem")

    def test_skip_role_writes_no_contract(self):
        rec = self._record(zeos_role="skip", signal_contract="",
                            mountpoint="", fstype="", mount_options="",
                            fstab_label="")
        self.assertIsNone(cfg_mod.write_signal_contract(rec))

    def test_remove_drops_record_and_contract(self):
        cfg = cfg_mod.load()
        rec = self._record()
        cfg_mod.upsert(cfg, rec)
        cfg_mod.save(cfg)
        cfg_mod.write_signal_contract(rec)
        cfg = cfg_mod.load()
        self.assertTrue(cfg_mod.remove(cfg, rec.identity()))
        cfg_mod.save(cfg)
        self.assertTrue(cfg_mod.remove_signal_contract(rec))
        self.assertEqual(cfg_mod.load()["devices"], [])


if __name__ == "__main__":
    unittest.main()
