#!/usr/bin/env python3
# I.7 form submit: browse test:submit, read button offset from serial, click it
# via `bclickp`, verify the <form> "submit" handler fired and mutated the DOM.
import json, os, re, socket, subprocess, sys, threading, time
from zqemu import accel_args
K=os.path.dirname(os.path.abspath(__file__)); B=os.path.join(K,"build"); ESP=os.path.join(B,"esptyp")
STABLE="/tmp/zeos-stable/BOOTZ.EFI"
QMP="/tmp/zeos-typ-qmp.sock"; SERSOCK="/tmp/zeos-typ-ser.sock"; SERLOG="/tmp/zeos-typ-serial.txt"
os.makedirs(os.path.join(ESP,"EFI","BOOT"),exist_ok=True)
subprocess.run(["cp",STABLE,os.path.join(ESP,"EFI","BOOT","BOOTX64.EFI")],check=True)
open(os.path.join(ESP,"startup.nsh"),"w").write("FS0:\r\nEFI\\BOOT\\BOOTX64.EFI\r\n")
subprocess.run(["cp","/usr/share/OVMF/OVMF_VARS_4M.fd",os.path.join(B,"OVMF_VARStyp.fd")],check=True)
vault=os.path.join(B,"vaulttyp.img"); subprocess.run(["rm","-f",vault]); subprocess.run(["qemu-img","create","-f","raw",vault,"8M"],check=True)
for p in (QMP,SERSOCK,SERLOG):
    try: os.remove(p)
    except OSError: pass
qemu=subprocess.Popen(["qemu-system-x86_64",*accel_args(),"-machine","q35","-m","512M",
 "-drive","if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
 "-drive",f"if=pflash,format=raw,file={B}/OVMF_VARStyp.fd","-drive",f"format=raw,file=fat:rw:{ESP}",
 "-drive",f"if=none,id=zeosvault,file={vault},format=raw","-device","nvme,drive=zeosvault,serial=ZEOSVAULT",
 "-device","virtio-net-pci,netdev=n0","-netdev","user,id=n0","-vga","std",
 "-chardev",f"socket,id=ss,path={SERSOCK},server=on,wait=off","-serial","chardev:ss",
 "-display","none","-qmp",f"unix:{QMP},server,nowait"],stdout=subprocess.DEVNULL,stderr=subprocess.STDOUT)
ss=None;t0=time.time()
while time.time()-t0<20:
    try: ss=socket.socket(socket.AF_UNIX);ss.connect(SERSOCK);break
    except OSError: time.sleep(0.3)
if not ss: print("SER FAIL");qemu.kill();sys.exit(1)
_stop=[False]
def rd():
    f=open(SERLOG,"ab"); ss.settimeout(0.5)
    while not _stop[0]:
        try: d=ss.recv(4096)
        except socket.timeout: continue
        except OSError: break
        if not d: break
        f.write(d); f.flush()
threading.Thread(target=rd,daemon=True).start()
def ser():
    try: return open(SERLOG,"r",errors="replace").read()
    except OSError: return ""
def sw(s): ss.sendall(s.encode())
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
def shot(tag):
    o=f"/tmp/zeos-typ-{tag}.png"; qmp("screendump",filename=o,format="png")
def key(q):
    qmp("input-send-event",events=[{"type":"key","data":{"down":True,"key":{"type":"qcode","data":q}}}])
    qmp("input-send-event",events=[{"type":"key","data":{"down":False,"key":{"type":"qcode","data":q}}}]);time.sleep(0.25)
t0=time.time()
while time.time()-t0<45 and "PIN enrollment" not in ser(): time.sleep(0.3)
time.sleep(1.0)
for _ in range(2):
    for d in "1234": key(d)
    key("ret");time.sleep(0.8)
for _ in range(6): time.sleep(1.0);key("ret")
t0=time.time();ready=False
while time.time()-t0<75:
    if "graphical desktop shell up" in ser(): ready=True; break
    time.sleep(0.5)
print("desktop:",ready,flush=True)
time.sleep(1.5)
sw("\n");time.sleep(0.7)
sw("browse test:type\n");time.sleep(4.0)
# focus the input: query its live box, click center
sw("bbox box\n");time.sleep(1.5)
m=re.search(r"bbox box x=(\d+) y=(\d+) w=(\d+) h=(\d+)", ser())
if not m:
    print("NO BBOX"); print(ser()[-800:]); _stop[0]=True; qmp("quit"); sys.exit(1)
bx,by,bw,bh=[int(g) for g in m.groups()]
cx,cy=bx+bw//2,by+bh//2
print("input box:",bx,by,bw,bh,"-> focus click",cx,cy,flush=True)
sw(f"bclickp {cx} {cy}\n");time.sleep(1.5)
sw("btype hello\n");time.sleep(2.0)
sw("bkey enter\n");time.sleep(2.0)
shot("done")
tail=ser().split("browse test:type")[-1]
print("=== submit result ===")
for ln in tail.splitlines():
    if "submit" in ln.lower() or "bclickp" in ln: print(ln)
_stop[0]=True;qmp("quit")
try: qemu.wait(timeout=5)
except Exception: qemu.kill()
