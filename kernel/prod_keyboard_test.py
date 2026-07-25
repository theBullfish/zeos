#!/usr/bin/env python3
# E.7 / production-path test: boot the PRODUCTION binary (NO lockscreen bypass)
# and drive the cold-boot PIN enrollment purely via injected KEYBOARD events
# over QMP. If keyboard IRQ delivery works (A.8 IOAPIC fix), the PIN gate is
# satisfiable and we reach the desktop on the real production path for the
# first time. Reuses vshot.py's proven QMP boot harness.
import json, os, socket, subprocess, sys, time

K=os.path.dirname(os.path.abspath(__file__)); B=os.path.join(K,"build"); ESP=os.path.join(B,"esp")
QMP="/tmp/zeos-x86-qmp.sock"; SER="/tmp/zeos-x86-serial.txt"
os.makedirs(os.path.join(ESP,"EFI","BOOT"),exist_ok=True)
subprocess.run(["cp",os.path.join(B,"BOOTZ.EFI"),os.path.join(ESP,"EFI","BOOT","BOOTX64.EFI")],check=True)
if not os.path.exists(os.path.join(B,"OVMF_VARS.fd")):
    subprocess.run(["cp","/usr/share/OVMF/OVMF_VARS_4M.fd",os.path.join(B,"OVMF_VARS.fd")],check=True)
vault=os.path.join(B,"vault.img")
if not os.path.exists(vault): subprocess.run(["qemu-img","create","-f","raw",vault,"8M"],check=True)
for p in (QMP,SER):
    try: os.remove(p)
    except OSError: pass

qemu=subprocess.Popen([
    "qemu-system-x86_64","-machine","q35","-m","512M",
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
def shot(tag):
    o=f"/tmp/zeos-p-{tag}.png"; cmd("screendump",filename=o,format="png")
    print(f"[{tag}] {os.path.getsize(o) if os.path.exists(o) else 0} bytes")
def key(qcode):
    cmd("input-send-event",events=[{"type":"key","data":{"down":True,"key":{"type":"qcode","data":qcode}}}])
    time.sleep(0.03)
    cmd("input-send-event",events=[{"type":"key","data":{"down":False,"key":{"type":"qcode","data":qcode}}}])
    time.sleep(0.08)

def ser():
    try: return open(SER,"rb").read().decode("latin-1")
    except OSError: return ""

# wait for the cold-boot PIN gate
t0=time.time(); gate=False
while time.time()-t0<40:
    if "cold-boot gate: PIN enrollment" in ser() or "PIN enrollment" in ser(): gate=True; break
    time.sleep(0.3)
print("PIN enrollment gate reached:", gate)
time.sleep(1.5)
shot("0-lockscreen")

# Enrollment: type 1234 <ret>, then 1234 <ret> (confirm)
for _ in range(2):
    for d in ("1","2","3","4"): key(d)
    key("ret")
    time.sleep(0.6)
time.sleep(2.0)
shot("1-after-pin")

# give the desktop a moment to compose
time.sleep(2.0)
shot("2-desktop")

cmd("quit")
try: qemu.wait(timeout=5)
except Exception: qemu.kill()
print("--- serial tail ---")
print(ser()[-900:])
