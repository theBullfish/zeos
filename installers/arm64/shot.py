#!/usr/bin/env python3
# Boot the Zeos aarch64 kernel under QEMU with ramfb, wait for the boot screen
# to render, then grab a PNG screenshot over QMP.
import json, os, socket, subprocess, sys, time

ELF   = "zeos-aarch64.elf"
SERIAL= "/tmp/zeos-serial.txt"
QMP   = "/tmp/zeos-qmp.sock"
PNG   = "/tmp/zeos-boot.png"

for p in (SERIAL, QMP, PNG):
    try: os.remove(p)
    except OSError: pass

qemu = subprocess.Popen([
    "qemu-system-aarch64",
    "-machine", "virt,gic-version=3",
    "-cpu", "cortex-a72", "-smp", "2", "-m", "512",
    "-kernel", ELF,
    "-dtb", "/tmp/virt.dtb",
    "-device", "ramfb",
    "-serial", f"file:{SERIAL}",
    "-display", "none",
    "-qmp", f"unix:{QMP},server,nowait",
], stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)

def wait_for(marker, timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            with open(SERIAL) as f:
                if marker in f.read(): return True
        except OSError: pass
        time.sleep(0.1)
    return False

# connect QMP
sock = None
t0 = time.time()
while time.time() - t0 < 10:
    try:
        sock = socket.socket(socket.AF_UNIX); sock.connect(QMP); break
    except OSError:
        time.sleep(0.1)
if not sock:
    print("QMP connect FAILED"); qemu.kill(); sys.exit(1)

f = sock.makefile("rw")
f.readline()                                   # greeting
f.write(json.dumps({"execute": "qmp_capabilities"}) + "\n"); f.flush(); f.readline()

rendered = wait_for("PIXELS ON SCREEN", 15)
time.sleep(0.5)                                # let the last draw settle

f.write(json.dumps({"execute": "screendump",
                    "arguments": {"filename": PNG, "format": "png"}}) + "\n")
f.flush()
print("screendump ->", f.readline().strip())

f.write(json.dumps({"execute": "quit"}) + "\n"); f.flush()
try: qemu.wait(timeout=5)
except Exception: qemu.kill()

print("rendered marker seen:", rendered)
print("serial tail:")
try:
    with open(SERIAL) as fh:
        print("".join(fh.readlines()[-12:]))
except OSError as e:
    print("  (no serial)", e)
print("PNG:", PNG, os.path.getsize(PNG) if os.path.exists(PNG) else "MISSING")
