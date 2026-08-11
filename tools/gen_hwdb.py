#!/usr/bin/env python3
"""
Generate os/boot/hwdb.c — Zeos's preloaded hardware knowledge base.

Zeos discovers what a board HAS at runtime (device tree / PCI / USB enumeration).
This gives it the other half: knowing what those IDs MEAN, with no network and no
lookup service, on first contact with a machine it has never seen.

Input : pci.ids / usb.ids (the canonical community ID databases)
Output: a single C file with a deduplicated string pool plus sorted index tables,
        looked up by binary search. No allocation, no parsing at runtime.

Usage: python3 tools/gen_hwdb.py [--pci PATH] [--usb PATH] -o os/boot/hwdb.c
"""
import argparse, sys

def parse_classes(path):
    """-> {(class, sub, progif): name} with None for unspecified levels.
    The class/subclass/prog-if triple is a STANDARDIZED contract (e.g. 01/08/02
    means 'speaks NVMe' no matter who made the chip), so this is the part that
    tells Zeos how to DRIVE a device it has never seen — not just name it."""
    out, c, sub = {}, None, None
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            if line.startswith("C "):
                try: c = int(line[2:4], 16)
                except ValueError: c = None; continue
                sub = None
                out[(c, None, None)] = line[6:].strip()
            elif c is not None and line.startswith("\t\t") and sub is not None:
                try: pi = int(line[2:4], 16)
                except ValueError: continue
                out[(c, sub, pi)] = line.strip()[3:].strip()
            elif c is not None and line.startswith("\t"):
                try: sub = int(line[1:3], 16)
                except ValueError: sub = None; continue
                out[(c, sub, None)] = line.strip()[3:].strip()
            else:
                c = None
    return out

def parse_ids(path):
    """-> (vendors {id: name}, devices {(vid,did): name})"""
    vendors, devices, cur = {}, {}, None
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            if line.startswith("\t\t"):          # subsystem — skipped
                continue
            if line.startswith("\t"):            # device
                if cur is None:
                    continue
                part = line[1:].rstrip("\n")
                if len(part) < 6 or part[4] != " ":
                    continue
                try:
                    did = int(part[:4], 16)
                except ValueError:
                    continue
                devices[(cur, did)] = part[5:].strip()
            else:                                 # vendor
                part = line.rstrip("\n")
                if len(part) < 6 or part[4] != " ":
                    continue
                try:
                    cur = int(part[:4], 16)
                except ValueError:
                    cur = None
                    continue
                vendors[cur] = part[5:].strip()
    return vendors, devices

class Pool:
    """Deduplicated NUL-terminated string pool."""
    def __init__(self):
        self.buf = bytearray(b"\0")     # offset 0 = empty string
        self.map = {"": 0}
    def add(self, s):
        s = s.replace("\t", " ").strip()
        if s in self.map:
            return self.map[s]
        off = len(self.buf)
        self.buf += s.encode("utf-8", "replace") + b"\0"
        self.map[s] = off
        return off

def c_pool(buf, per_line=24):
    """Emit the pool as chunked C string literals (implicit concatenation)."""
    out, i = [], 0
    while i < len(buf):
        chunk = buf[i:i+per_line]
        out.append('  "' + "".join("\\%03o" % b for b in chunk) + '"')
        i += per_line
    return "\n".join(out)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pci", default="/usr/share/misc/pci.ids")
    ap.add_argument("--usb", default="/usr/share/misc/usb.ids")
    ap.add_argument("-o", "--out", required=True)
    a = ap.parse_args()

    pool = Pool()
    tables = {}
    for kind, path in (("pci", a.pci), ("usb", a.usb)):
        try:
            vendors, devices = parse_ids(path)
        except OSError as e:
            print(f"warning: {path}: {e} — {kind} table will be empty", file=sys.stderr)
            vendors, devices = {}, {}
        tables[kind] = (
            sorted((v, pool.add(n)) for v, n in vendors.items()),
            sorted(((v << 16) | d, pool.add(n)) for (v, d), n in devices.items()),
        )
        print(f"{kind}: {len(vendors)} vendors, {len(devices)} devices", file=sys.stderr)

    classes = parse_classes(a.pci)
    ctab = sorted((((c << 16) | ((0xFF if s_ is None else s_) << 8) |
                    (0xFF if p_ is None else p_)), pool.add(n))
                  for (c, s_, p_), n in classes.items())
    print(f"pci classes: {len(ctab)} entries", file=sys.stderr)

    with open(a.out, "w") as f:
        f.write("""/*
 * Zeos hardware knowledge base — GENERATED, do not edit.
 * Regenerate: python3 tools/gen_hwdb.py -o os/boot/hwdb.c
 *
 * Lets Zeos NAME any PCI/USB device it enumerates with no network and no lookup
 * service, on first contact with hardware it has never seen. Source data is the
 * canonical community pci.ids / usb.ids databases; this file is a deduplicated
 * string pool plus sorted index tables searched by bisection at runtime.
 */
#include <stdint.h>

""")
        f.write("static const char hwdb_pool[] =\n")
        f.write(c_pool(pool.buf))
        f.write(";\n\n")
        f.write("struct hwdb_v { uint16_t id; uint32_t off; };\n")
        f.write("struct hwdb_d { uint32_t key; uint32_t off; };\n\n")
        for kind in ("pci", "usb"):
            vt, dt = tables[kind]
            f.write(f"static const struct hwdb_v {kind}_vendors[] = {{\n")
            for i, (vid, off) in enumerate(vt):
                f.write(f"{{0x{vid:04x},{off}}},"); f.write("\n" if i % 6 == 5 else "")
            f.write("\n};\n")
            f.write(f"static const struct hwdb_d {kind}_devices[] = {{\n")
            for i, (key, off) in enumerate(dt):
                f.write(f"{{0x{key:08x},{off}}},"); f.write("\n" if i % 5 == 4 else "")
            f.write("\n};\n\n")
        f.write("static const struct hwdb_d pci_classes[] = {\n")
        for i, (key, off) in enumerate(ctab):
            f.write(f"{{0x{key:08x},{off}}},"); f.write("\n" if i % 5 == 4 else "")
        f.write("\n};\n\n")

        f.write("""
static const char *v_lookup(const struct hwdb_v *t, int n, uint16_t id)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (t[m].id == id) return hwdb_pool + t[m].off;
        if (t[m].id < id) lo = m + 1; else hi = m - 1;
    }
    return 0;
}
static const char *d_lookup(const struct hwdb_d *t, int n, uint32_t key)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (t[m].key == key) return hwdb_pool + t[m].off;
        if (t[m].key < key) lo = m + 1; else hi = m - 1;
    }
    return 0;
}
#define NELEM(a) ((int)(sizeof(a)/sizeof((a)[0])))

/* Human-readable names for an enumerated device, or NULL if unknown. */
const char *hwdb_pci_vendor(uint16_t vid) { return v_lookup(pci_vendors, NELEM(pci_vendors), vid); }
const char *hwdb_pci_device(uint16_t vid, uint16_t did)
{ return d_lookup(pci_devices, NELEM(pci_devices), ((uint32_t)vid << 16) | did); }
const char *hwdb_usb_vendor(uint16_t vid) { return v_lookup(usb_vendors, NELEM(usb_vendors), vid); }
const char *hwdb_usb_device(uint16_t vid, uint16_t did)
{ return d_lookup(usb_devices, NELEM(usb_devices), ((uint32_t)vid << 16) | did); }

/*
 * Class/subclass/prog-if -> standardized meaning. 0xFF = "unspecified level", so
 * we try most-specific first (triple), then subclass, then class.
 */
const char *hwdb_pci_class(uint8_t cls, uint8_t sub, uint8_t progif)
{
    const uint32_t tries[3] = {
        ((uint32_t)cls << 16) | ((uint32_t)sub << 8) | progif,
        ((uint32_t)cls << 16) | ((uint32_t)sub << 8) | 0xFF,
        ((uint32_t)cls << 16) | (0xFFu << 8) | 0xFF,
    };
    for (int i = 0; i < 3; i++) {
        const char *n = d_lookup(pci_classes, NELEM(pci_classes), tries[i]);
        if (n) return n;
    }
    return 0;
}

/*
 * The payoff: map a device to the PROTOCOL it speaks, from its class triple
 * alone. Any chip reporting these values implements that published spec no
 * matter who built it or when — so Zeos can bind the right driver to hardware
 * that did not exist when this image was built, with no probing.
 * Returns a short stable token, or 0 if the class implies no standard protocol.
 */
const char *hwdb_pci_protocol(uint8_t cls, uint8_t sub, uint8_t progif)
{
    switch (cls) {
    case 0x01:                                  /* mass storage */
        if (sub == 0x08 && progif == 0x02) return "nvme";
        if (sub == 0x06 && progif == 0x01) return "ahci";
        if (sub == 0x06)                   return "sata";
        if (sub == 0x01)                   return "ide";
        if (sub == 0x07)                   return "sas";
        return "storage";
    case 0x0C:                                  /* serial bus */
        if (sub == 0x03) {
            if (progif == 0x30) return "xhci";
            if (progif == 0x20) return "ehci";
            if (progif == 0x10) return "ohci";
            if (progif == 0x00) return "uhci";
            return "usb";
        }
        return 0;
    case 0x02: return "ethernet";
    case 0x03: return "display";
    case 0x04: return (sub == 0x03) ? "hda" : "audio";
    case 0x06: return "bridge";
    case 0x0D: return "wireless";
    default:   return 0;
    }
}

int hwdb_pci_class_count(void) { return NELEM(pci_classes); }

/* Coverage, for the boot log / `hwid` shell command. */
int hwdb_pci_vendor_count(void) { return NELEM(pci_vendors); }
int hwdb_pci_device_count(void) { return NELEM(pci_devices); }
int hwdb_usb_vendor_count(void) { return NELEM(usb_vendors); }
int hwdb_usb_device_count(void) { return NELEM(usb_devices); }
""")
    print(f"wrote {a.out}", file=sys.stderr)

main()
