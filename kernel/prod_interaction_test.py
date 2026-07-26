#!/usr/bin/env python3
# Nail the interaction 20% on the PRODUCTION path (KVM). Reaches the desktop via
# keyboard (PIN enroll + welcome), then exercises drag/focus/maximize/workspace,
# reading the [WM] serial state (ZEOS_DIAG_WM_STATE) after each action for
# ground-truth verification -- no pixel-guessing.
import json, os, socket, subprocess, sys, time, re
from zqemu import accel_args
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
    "qemu-system-x86_64",*accel_args(),"-machine","q35","-m","512M",
    "-drive","if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
    "-drive",f"if=pflash,format=raw,file={B}/OVMF_VARS.fd",
    "-drive",f"format=raw,file=fat:rw:{ESP}",
    "-drive",f"if=none,id=zeosvault,file={vault},format=raw","-device","nvme,drive=zeosvault,serial=ZEOSVAULT",
    "-vga","std","-net","none","-serial",f"file:{SER}","-display","none","-qmp",f"unix:{QMP},server,nowait",
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
    try: return open(SER,"rb").read().decode("latin-1")
    except OSError: return ""
def keyev(q,d): cmd("input-send-event",events=[{"type":"key","data":{"down":d,"key":{"type":"qcode","data":q}}}]); time.sleep(0.04)
def tap(q): keyev(q,True); keyev(q,False); time.sleep(0.08)
def chord(mods,k):
    for m in mods: keyev(m,True)
    keyev(k,True); time.sleep(0.04); keyev(k,False)
    for m in reversed(mods): keyev(m,False)
    time.sleep(0.5)
def rel(dx,dy):
    while dx or dy:
        sx=max(-100,min(100,dx)); sy=max(-100,min(100,dy)); dx-=sx; dy-=sy
        cmd("input-send-event",events=[{"type":"rel","data":{"axis":"x","value":sx}},{"type":"rel","data":{"axis":"y","value":sy}}]); time.sleep(0.03)
def btn(d): cmd("input-send-event",events=[{"type":"btn","data":{"button":"left","down":d}}]); time.sleep(0.12)
def last_wm():
    d=ser(); lines=d.splitlines(); block=[]
    for i in range(len(lines)-1,-1,-1):
        if lines[i].startswith("[WM] active_ws"):
            block=[lines[i]]
            j=i+1
            while j<len(lines) and lines[j].startswith("[WM]"): block.append(lines[j]); j+=1
            break
    return "\n".join(block)

# reach desktop: PIN enroll (1234 ret x2) + welcome persona (1 ret)
t0=time.time()
while time.time()-t0<40 and "PIN enrollment" not in ser(): time.sleep(0.2)
time.sleep(1.0)
for _ in range(2):
    for dgt in ("1","2","3","4"): tap(dgt)
    tap("ret"); time.sleep(0.5)
time.sleep(1.0); tap("1"); time.sleep(0.2); tap("ret")   # welcome persona
t0=time.time()
while time.time()-t0<30 and "chain-resolution main loop" not in ser(): time.sleep(0.2)
time.sleep(2.0)
print("=== reached desktop:", "chain-resolution main loop" in ser())
print("--- BASELINE ---"); print(last_wm())

# C.3 drag: cursor from center onto Terminal titlebar, grab, drag, drop
rel(1100-960, 335-540); time.sleep(0.2); btn(True); rel(180,140); btn(False); time.sleep(0.5)
print("--- after DRAG (C.3) ---"); print(last_wm())

# C.2 focus/z: click occluded Files (top-left visible area)
rel(300-1280, 400-680); time.sleep(0.2); btn(True); btn(False); time.sleep(0.5)
print("--- after CLICK Files (C.2) ---"); print(last_wm())

# C.5 maximize the focused window
chord(["meta_l"],"up"); time.sleep(0.6)
print("--- after Super+Up (C.5) ---"); print(last_wm())

# C.6 workspace switch to ws2 then back
chord(["meta_l","ctrl"],"2"); time.sleep(0.6)
print("--- after Super+Ctrl+2 (C.6 -> ws2) ---"); print(last_wm())
chord(["meta_l","ctrl"],"1"); time.sleep(0.6)
print("--- after Super+Ctrl+1 (C.6 -> ws1) ---"); print(last_wm())

cmd("quit")
try: qemu.wait(timeout=5)
except Exception: qemu.kill()
