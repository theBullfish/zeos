#!/usr/bin/env python3
# Boot the x86 Zeos desktop (BOOTZ.EFI) under QEMU+OVMF headless, drive the
# lockscreen via QMP key events, and capture PNGs at each stage.
import json, os, socket, subprocess, sys, time

K   = os.path.dirname(os.path.abspath(__file__))
B   = os.path.join(K, "build")
ESP = os.path.join(B, "esp")
QMP = "/tmp/zeos-x86-qmp.sock"
SER = "/tmp/zeos-x86-serial.txt"

os.makedirs(os.path.join(ESP, "EFI", "BOOT"), exist_ok=True)
subprocess.run(["cp", os.path.join(B, "BOOTZ.EFI"),
                os.path.join(ESP, "EFI", "BOOT", "BOOTX64.EFI")], check=True)
if not os.path.exists(os.path.join(B, "OVMF_VARS.fd")):
    subprocess.run(["cp", "/usr/share/OVMF/OVMF_VARS_4M.fd",
                    os.path.join(B, "OVMF_VARS.fd")], check=True)
vault = os.path.join(B, "vault.img")
if not os.path.exists(vault):
    subprocess.run(["qemu-img", "create", "-f", "raw", vault, "8M"], check=True)
for p in (QMP, SER):
    try: os.remove(p)
    except OSError: pass

qemu = subprocess.Popen([
    "qemu-system-x86_64",
    "-machine", "q35", "-m", "512M",
    "-drive", "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
    "-drive", f"if=pflash,format=raw,file={B}/OVMF_VARS.fd",
    "-drive", f"format=raw,file=fat:rw:{ESP}",
    "-drive", f"if=none,id=zeosvault,file={vault},format=raw",
    "-device", "nvme,drive=zeosvault,serial=ZEOSVAULT",
    "-vga", "std",
    "-serial", f"file:{SER}",
    "-display", "none",
    "-qmp", f"unix:{QMP},server,nowait",
], stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)

sock = None; t0 = time.time()
while time.time() - t0 < 15:
    try:
        sock = socket.socket(socket.AF_UNIX); sock.connect(QMP); break
    except OSError: time.sleep(0.2)
if not sock:
    print("QMP connect FAILED"); qemu.kill(); sys.exit(1)
f = sock.makefile("rw")

def cmd(execute, **args):
    msg = {"execute": execute}
    if args: msg["arguments"] = args
    f.write(json.dumps(msg) + "\n"); f.flush()
    while True:                                   # drain async events until return/error
        line = f.readline()
        if not line: return None
        obj = json.loads(line)
        if "return" in obj or "error" in obj: return obj

f.readline()                                       # greeting
cmd("qmp_capabilities")

def shot(tag):
    out = f"/tmp/zeos-x86-{tag}.png"
    r = cmd("screendump", filename=out, format="png")
    sz = os.path.getsize(out) if os.path.exists(out) else 0
    print(f"[shot {tag}] {sz} bytes  {r}")
    return sz

def key(*names):
    for n in names:
        r = cmd("send-key", keys=[{"type": "qcode", "data": n}], **{"hold-time": 80})
        if r and "error" in r: print("  send-key err", n, r)
        time.sleep(0.12)

time.sleep(float(sys.argv[1]) if len(sys.argv) > 1 else 12)
shot("A-boot")                                     # whatever is on screen after boot

def mouse_jiggle(dx=40, dy=25):
    cmd("input-send-event", events=[
        {"type": "rel", "data": {"axis": "x", "value": dx}},
        {"type": "rel", "data": {"axis": "y", "value": dy}}])

# cold-boot PIN enrollment: type a PIN, enter, repeat to confirm
key("1","2","3","4"); key("ret"); time.sleep(1.2)
key("1","2","3","4"); key("ret"); time.sleep(3.0)
shot("welcome")                                    # mode picker
# choose [3] Full -- everything, curtain raised
key("3"); time.sleep(0.5); key("ret"); time.sleep(2.5)

# poll frames while nudging input, to catch the desktop repaint
for i in range(12):
    mouse_jiggle(37 if i % 2 == 0 else -37, 21)
    if i % 4 == 1: key("esc")
    time.sleep(1.3)
    shot(f"F{i:02d}")

f2 = None
cmd("quit")
try: qemu.wait(timeout=5)
except Exception: qemu.kill()

print("--- serial tail ---")
try:
    with open(SER, "rb") as fh: data = fh.read().decode("latin-1")
    print(data[-1200:] if data else "(empty)")
except OSError as e:
    print("(no serial)", e)
