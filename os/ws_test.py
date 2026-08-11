#!/usr/bin/env python3
# C.5 + E.8: window management via KEYBOARD shortcuts (precise, no pointer error).
# Super+Up = maximize the focused window. Boot (harness), snapshot, inject the
# Super+Up chord, snapshot -> the focused Terminal should fill the screen.
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
    o=f"/tmp/zeos-ws-{tag}.png"; cmd("screendump",filename=o,format="png")
    print(f"[{tag}] {os.path.getsize(o) if os.path.exists(o) else 0} bytes")
def keyev(q,down):
    cmd("input-send-event",events=[{"type":"key","data":{"down":down,"key":{"type":"qcode","data":q}}}])
    time.sleep(0.05)
def chord(mod,k):
    keyev(mod,True); keyev(k,True); time.sleep(0.05); keyev(k,False); keyev(mod,False); time.sleep(0.4)
def chord2(m1,m2,k):
    keyev(m1,True); keyev(m2,True); keyev(k,True); time.sleep(0.05)
    keyev(k,False); keyev(m2,False); keyev(m1,False); time.sleep(0.6)
t0=time.time(); ready=False
while time.time()-t0<30:
    try:
        if "chain-resolution main loop" in open(SER).read(): ready=True; break
    except OSError: pass
    time.sleep(0.3)
print("scheduler loop up:", ready)
time.sleep(2)
shot("0-boot")           # workspace 1: Files + Terminal
chord2("meta_l","ctrl","2")   # Super+Ctrl+2 -> switch to workspace 2 (empty)
time.sleep(0.8)
shot("1-ws2")           # should be empty (windows are on ws1)
chord2("meta_l","ctrl","1")   # Super+Ctrl+1 -> back to workspace 1
time.sleep(0.8)
shot("2-ws1-back")      # windows should return
cmd("quit")
try: qemu.wait(timeout=5)
except Exception: qemu.kill()
