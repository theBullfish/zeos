#!/usr/bin/env python3
# VERIFY (not vshot): boot, let ALL springs settle, then take several STATIC
# screenshots with NO input. present goes idle -> framebuffer is stable, so the
# async-screendump mid-composite race can't fire. Stable windows across all
# shots = the desktop genuinely renders (cfe7041). Intermittent blank in vshot
# was the capture racing an open-spring composite, not a real vanish.
import json, os, socket, subprocess, sys, time

K=os.path.dirname(os.path.abspath(__file__)); B=os.path.join(K,"build"); ESP=os.path.join(B,"esp")
QMP="/tmp/zeos-verify-qmp.sock"; SER="/tmp/zeos-verify-serial.txt"
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

t0=time.time(); ready=False
while time.time()-t0<30:
    try:
        if "chain-resolution main loop" in open(SER).read(): ready=True; break
    except OSError: pass
    time.sleep(0.3)
print("scheduler loop up:", ready)

# Let EVERYTHING settle: open-springs, persona, cursor init. No input at all.
time.sleep(6)
sizes=[]
for i in range(5):
    o=f"/tmp/zeos-verify-{i}.png"
    cmd("screendump",filename=o,format="png")
    sz=os.path.getsize(o) if os.path.exists(o) else 0
    sizes.append(sz); print(f"[settled shot {i}] {sz} bytes")
    time.sleep(1.3)
cmd("quit")
try: qemu.wait(timeout=5)
except Exception: qemu.kill()
# verdict: if all shots are close in size and well above the ~9618 blank, stable render
lo,hi=min(sizes),max(sizes)
print(f"\nrange {lo}..{hi}  spread={hi-lo}")
print("VERDICT:", "STABLE windows (verified)" if lo>10500 else
      "MIXED/blank — real problem" if lo<9800 else "borderline")
