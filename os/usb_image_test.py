#!/usr/bin/env python3
# Pre-flight for physical hardware: boot the REAL GPT USB image (build/zeos-usb.img)
# as a raw USB mass-storage device, with NO startup.nsh and NO fat:rw: directory
# shortcut. This exercises the same path a physical machine takes: firmware finds
# the ESP by GPT type GUID and auto-launches the removable fallback
# \EFI\BOOT\BOOTX64.EFI. If this fails, the image is wrong; if it passes, any
# failure on metal is a hardware/firmware difference, not a packaging bug.
import json, os, socket, subprocess, sys, threading, time
from zqemu import accel_args
K=os.path.dirname(os.path.abspath(__file__)); B=os.path.join(K,"build")
IMG=os.path.join(B,"zeos-usb.img")
QMP="/tmp/zeos-usb-qmp.sock"; SER="/tmp/zeos-usb-ser.sock"; LOG="/tmp/zeos-usb-serial.txt"
for p in (QMP,SER,LOG):
    try: os.remove(p)
    except OSError: pass
subprocess.run(["cp","/usr/share/OVMF/OVMF_VARS_4M.fd",os.path.join(B,"OVMF_VARSusb.fd")],check=True)
qemu=subprocess.Popen(["qemu-system-x86_64",*accel_args(),"-machine","q35","-m","512M",
 "-drive","if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
 "-drive",f"if=pflash,format=raw,file={B}/OVMF_VARSusb.fd",
 # present the image as a real USB mass-storage stick, like the thumb drive
 # READ-ONLY. Attaching the image writable let the guest (OVMF NvVars / Zeos
 # persistence) write into it and ZERO the primary GPT at LBA1 — the image
 # booted fine here, then every dd afterwards faithfully copied a corrupted
 # image to the stick. A pre-flight test must never mutate the artifact it is
 # certifying.
 "-drive",f"if=none,id=usbstick,format=raw,file={IMG},readonly=on",
 "-device","qemu-xhci,id=xhci","-device","usb-storage,bus=xhci.0,drive=usbstick",
 "-device","virtio-net-pci,netdev=n0","-netdev","user,id=n0","-vga","std",
 "-chardev",f"socket,id=ss,path={SER},server=on,wait=off","-serial","chardev:ss",
 "-display","none","-qmp",f"unix:{QMP},server,nowait"],stdout=subprocess.DEVNULL,stderr=subprocess.STDOUT)
ss=None;t0=time.time()
while time.time()-t0<20:
    try: ss=socket.socket(socket.AF_UNIX);ss.connect(SER);break
    except OSError: time.sleep(0.3)
if not ss: print("SER FAIL");qemu.kill();sys.exit(1)
stop=[False]
def rd():
    f=open(LOG,"ab"); ss.settimeout(0.5)
    while not stop[0]:
        try: d=ss.recv(4096)
        except socket.timeout: continue
        except OSError: break
        if not d: break
        f.write(d); f.flush()
threading.Thread(target=rd,daemon=True).start()
def ser():
    try: return open(LOG,errors="replace").read()
    except OSError: return ""
qs=None;t0=time.time()
while time.time()-t0<15:
    try: qs=socket.socket(socket.AF_UNIX);qs.connect(QMP);break
    except OSError: time.sleep(0.3)
qf=qs.makefile("rw")
def qmp(e,**a):
    m={"execute":e}
    if a:m["arguments"]=a
    qf.write(json.dumps(m)+"\n");qf.flush()
    while True:
        l=qf.readline()
        if not l:return
        o=json.loads(l)
        if "return" in o or "error" in o:return o
qf.readline();qmp("qmp_capabilities")
def key(q):
    qmp("input-send-event",events=[{"type":"key","data":{"down":True,"key":{"type":"qcode","data":q}}}])
    qmp("input-send-event",events=[{"type":"key","data":{"down":False,"key":{"type":"qcode","data":q}}}]);time.sleep(0.25)
# wait for the kernel to say anything at all (proves firmware auto-launched us)
t0=time.time(); launched=False
while time.time()-t0<60:
    if "Zeos" in ser() or "[main]" in ser() or "PIN" in ser(): launched=True; break
    time.sleep(0.5)
print("kernel launched from GPT image:",launched,flush=True)
t0=time.time()
while time.time()-t0<45 and "PIN enrollment" not in ser(): time.sleep(0.3)
time.sleep(1.0)
for _ in range(2):
    for d in "1234": key(d)
    key("ret");time.sleep(0.8)
for _ in range(6): time.sleep(1.0);key("ret")
t0=time.time(); desk=False
while time.time()-t0<75:
    if "graphical desktop shell up" in ser(): desk=True;break
    time.sleep(0.5)
print("desktop:",desk,flush=True)
time.sleep(2.0)
qmp("screendump",filename="/tmp/zeos-usb-boot.png",format="png")
stop[0]=True;qmp("quit")
try: qemu.wait(timeout=5)
except Exception: qemu.kill()
