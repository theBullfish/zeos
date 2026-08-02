#!/usr/bin/env python3
# Boot-to-desktop harness for the BUILD_MAP verification sweep.
# Drives cold-boot PIN enrollment + the 5-screen first-boot wizard (arrow/enter),
# reaches the live desktop, and captures a battery of interaction screenshots.
# Headless via QMP. Screens saved to /tmp/zeos-s-*.png, serial to /tmp/zeos-s-serial.txt
import json, os, socket, subprocess, sys, time
from zqemu import accel_args

K=os.path.dirname(os.path.abspath(__file__)); B=os.path.join(K,"build"); ESP=os.path.join(B,"esp")
QMP="/tmp/zeos-s-qmp.sock"; SER="/tmp/zeos-s-serial.txt"
os.makedirs(os.path.join(ESP,"EFI","BOOT"),exist_ok=True)
subprocess.run(["cp",os.path.join(B,"BOOTZ.EFI"),os.path.join(ESP,"EFI","BOOT","BOOTX64.EFI")],check=True)
subprocess.run(["cp","/usr/share/OVMF/OVMF_VARS_4M.fd",os.path.join(B,"OVMF_VARS.fd")],check=True)
vault=os.path.join(B,"vault.img")
subprocess.run(["rm","-f",vault]); subprocess.run(["qemu-img","create","-f","raw",vault,"8M"],check=True)
for p in (QMP,SER):
    try: os.remove(p)
    except OSError: pass

qemu=subprocess.Popen([
    "qemu-system-x86_64", *accel_args(), "-machine","q35","-m","512M",
    "-drive","if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
    "-drive",f"if=pflash,format=raw,file={B}/OVMF_VARS.fd",
    "-drive",f"format=raw,file=fat:rw:{ESP}",
    "-drive",f"if=none,id=zeosvault,file={vault},format=raw",
    "-device","nvme,drive=zeosvault,serial=ZEOSVAULT",
    "-vga","std","-net","none",
    "-serial",f"file:{SER}","-display","none","-qmp",f"unix:{QMP},server,nowait",
],stdout=subprocess.DEVNULL,stderr=subprocess.STDOUT)

sock=None;t0=time.time()
while time.time()-t0<15:
    try: sock=socket.socket(socket.AF_UNIX);sock.connect(QMP);break
    except OSError: time.sleep(0.2)
if not sock: print("QMP FAIL");qemu.kill();sys.exit(1)
f=sock.makefile("rw")
def cmd(e,**a):
    m={"execute":e}
    if a:m["arguments"]=a
    f.write(json.dumps(m)+"\n");f.flush()
    while True:
        l=f.readline()
        if not l: return None
        o=json.loads(l)
        if "return" in o or "error" in o: return o
f.readline(); cmd("qmp_capabilities")
def ser():
    try: return open(SER,"r",errors="replace").read()
    except OSError: return ""
def shot(tag):
    o=f"/tmp/zeos-s-{tag}.png"; cmd("screendump",filename=o,format="png")
    print(f"[{tag}] {os.path.getsize(o) if os.path.exists(o) else 0} bytes", flush=True)
def key(q):
    cmd("input-send-event",events=[{"type":"key","data":{"down":True,"key":{"type":"qcode","data":q}}}])
    cmd("input-send-event",events=[{"type":"key","data":{"down":False,"key":{"type":"qcode","data":q}}}])
    time.sleep(0.25)
def rel(dx,dy):
    while dx or dy:
        sx=max(-100,min(100,dx));sy=max(-100,min(100,dy));dx-=sx;dy-=sy
        cmd("input-send-event",events=[{"type":"rel","data":{"axis":"x","value":sx}},{"type":"rel","data":{"axis":"y","value":sy}}]); time.sleep(0.03)
def btn(down):
    cmd("input-send-event",events=[{"type":"btn","data":{"button":"left","down":down}}]); time.sleep(0.1)

# 1. Wait for PIN enrollment gate
t0=time.time(); gate=False
while time.time()-t0<40:
    if "PIN enrollment" in ser(): gate=True; break
    time.sleep(0.3)
print("PIN gate:", gate, flush=True)
time.sleep(1.0)
# 2. Enroll PIN 1234 (twice: set + confirm)
for _ in range(2):
    for d in ("1","2","3","4"): key(d)
    key("ret"); time.sleep(0.8)
shot("after-pin")
# 3. Probe the first-boot wizard: screenshot, press enter, repeat.
for i in range(6):
    time.sleep(1.2)
    shot(f"wiz-{i}")
    key("ret")
# 4. Wait for the live desktop / scheduler loop
t0=time.time(); ready=False
while time.time()-t0<30:
    if "chain-resolution main loop" in ser(): ready=True; break
    time.sleep(0.3)
print("scheduler loop up:", ready, flush=True)
time.sleep(3.0)
shot("desktop")

# ── Interaction battery (each screenshot is an evidence artifact) ──
# Cursor motion + click feedback (E.2/E.3)
rel(-200,-150); shot("cursor-moved")
btn(True); time.sleep(0.15); btn(False); shot("cursor-click")
# Command palette via Super+Space (E.8 keybinds, J.3 search palette)
def combo(mods, k):
    for m in mods: cmd("input-send-event",events=[{"type":"key","data":{"down":True,"key":{"type":"qcode","data":m}}}])
    cmd("input-send-event",events=[{"type":"key","data":{"down":True,"key":{"type":"qcode","data":k}}}])
    cmd("input-send-event",events=[{"type":"key","data":{"down":False,"key":{"type":"qcode","data":k}}}])
    for m in reversed(mods): cmd("input-send-event",events=[{"type":"key","data":{"down":False,"key":{"type":"qcode","data":m}}}])
    time.sleep(0.4)
combo(["meta_l"], "spc"); time.sleep(0.6); shot("palette"); key("esc"); time.sleep(0.3)
# Workspace switch Super+2 then Super+1 (C.6)
combo(["meta_l"], "2"); time.sleep(0.6); shot("workspace-2")
combo(["meta_l"], "1"); time.sleep(0.4)
# Titlebar drag of the focused (Terminal) window (C.3): grab near its titlebar, drag
rel(300,-250); time.sleep(0.2)            # move toward a titlebar region
btn(True); rel(-150,120); btn(False); shot("after-drag")
# Maximize via Super+Up (E.8 / C.5)
combo(["meta_l"], "up"); time.sleep(0.5); shot("maximize")

cmd("quit")
try: qemu.wait(timeout=5)
except Exception: qemu.kill()
print("--- serial tail ---")
print(ser()[-1200:])
