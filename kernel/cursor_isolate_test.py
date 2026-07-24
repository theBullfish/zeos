#!/usr/bin/env python3
# Isolate: does QMP relative motion move the cursor AT ALL, independent of drag?
import json, os, socket, subprocess, sys, time

K=os.path.dirname(os.path.abspath(__file__)); B=os.path.join(K,"build"); ESP=os.path.join(B,"esp")
QMP="/tmp/zeos-iso-qmp.sock"; SER="/tmp/zeos-iso-serial.txt"
os.makedirs(os.path.join(ESP,"EFI","BOOT"),exist_ok=True)
subprocess.run(["cp",os.path.join(B,"BOOTZ.EFI"),os.path.join(ESP,"EFI","BOOT","BOOTX64.EFI")],check=True)
vault=os.path.join(B,"vault.img")
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
    o=f"/tmp/zeos-iso-{tag}.png"; r=cmd("screendump",filename=o,format="png")
    print(f"[{tag}] {os.path.getsize(o) if os.path.exists(o) else 0} bytes  qmp_return={r}")
    return o

t0=time.time(); ready=False
while time.time()-t0 < 30:
    try:
        if "chain-resolution main loop" in open(SER).read(): ready=True; break
    except OSError: pass
    time.sleep(0.3)
print("scheduler loop up:", ready)
time.sleep(2)
shot("A-before")

# Send a LARGE, unmistakable, absolute-style repeated relative motion: many
# small steps far in one direction so even a scale/clamp issue can't hide it.
for _ in range(20):
    r = cmd("input-send-event", events=[
        {"type":"rel","data":{"axis":"x","value":40}},
        {"type":"rel","data":{"axis":"y","value":0}}])
    time.sleep(0.05)
print("last input-send-event qmp return:", r)
time.sleep(0.5)
shot("B-after-400px-right")

cmd("quit")
try: qemu.wait(timeout=5)
except Exception: qemu.kill()
print("--- serial tail (mouse/input related) ---")
try:
    txt = open(SER,"rb").read().decode("latin-1")
    for line in txt.splitlines():
        if any(k in line.lower() for k in ["mouse","ps2","cursor","input","irq"]):
            print(line)
except OSError as e:
    print("(no serial)", e)
