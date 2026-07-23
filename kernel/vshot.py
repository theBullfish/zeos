#!/usr/bin/env python3
# Verify the live/interactive desktop: boot to desktop, inject mouse motion +
# a titlebar drag via QMP, screenshot each stage. Proves cursor renders/moves
# and wm_mouse_down/move/up drive window focus+drag.
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
    "-vga","std","-net","none",   # no net -> DHCP can't stall the boot to scheduler_run
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
    o=f"/tmp/zeos-v-{tag}.png"; cmd("screendump",filename=o,format="png")
    print(f"[{tag}] {os.path.getsize(o) if os.path.exists(o) else 0} bytes")
def rel(dx,dy):
    # send in <=100px chunks (PS/2 packet range) so nothing is dropped
    while dx or dy:
        sx=max(-100,min(100,dx)); sy=max(-100,min(100,dy)); dx-=sx; dy-=sy
        cmd("input-send-event",events=[
            {"type":"rel","data":{"axis":"x","value":sx}},
            {"type":"rel","data":{"axis":"y","value":sy}}])
        time.sleep(0.03)
def btn(down):
    cmd("input-send-event",events=[{"type":"btn","data":{"button":"left","down":down}}])
    time.sleep(0.1)

# wait until the live scheduler loop is actually running
t0=time.time(); ready=False
while time.time()-t0 < 30:
    try:
        if "chain-resolution main loop" in open(SER).read(): ready=True; break
    except OSError: pass
    time.sleep(0.3)
print("scheduler loop up:", ready)
time.sleep(2)                       # let window-open springs settle
shot("0-boot")                      # cursor at (0,0) corner, mouse at center (960,540)

rel(0,-205)                         # move up onto the Terminal titlebar (~960,335)
time.sleep(0.4); shot("1-cursor")   # cursor should now be visible on the titlebar

btn(True)                           # grab titlebar (focus + drag start)
rel(250,240)                        # drag the window down-right
btn(False)                          # drop
time.sleep(0.4); shot("2-dragged")  # Terminal window should have moved

cmd("quit")
try: qemu.wait(timeout=5)
except Exception: qemu.kill()
print("--- serial tail ---")
try: print(open(SER,"rb").read().decode("latin-1")[-600:])
except OSError as e: print("(no serial)",e)
