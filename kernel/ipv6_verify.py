#!/usr/bin/env python3
# Isolated IPv6 verification — uses a STABLE BOOTZ.EFI copy (immune to the other
# assistant's concurrent build clobbering build/). Boots, waits for DHCP, then
# drives dns/ping (v4 regression check) + ping6 (SLAAC/v6). ipv6=on in slirp.
import json, os, socket, subprocess, sys, threading, time
from zqemu import accel_args
K=os.path.dirname(os.path.abspath(__file__)); B=os.path.join(K,"build"); ESP=os.path.join(B,"esp6")
STABLE="/tmp/zeos-stable/BOOTZ.EFI"
QMP="/tmp/zeos-i6-qmp.sock"; SERSOCK="/tmp/zeos-i6-ser.sock"; SERLOG="/tmp/zeos-i6-serial.txt"
os.makedirs(os.path.join(ESP,"EFI","BOOT"),exist_ok=True)
subprocess.run(["cp",STABLE,os.path.join(ESP,"EFI","BOOT","BOOTX64.EFI")],check=True)
open(os.path.join(ESP,"startup.nsh"),"w").write("FS0:\r\nEFI\\BOOT\\BOOTX64.EFI\r\n")
subprocess.run(["cp","/usr/share/OVMF/OVMF_VARS_4M.fd",os.path.join(B,"OVMF_VARS6.fd")],check=True)
vault=os.path.join(B,"vault6.img"); subprocess.run(["rm","-f",vault]); subprocess.run(["qemu-img","create","-f","raw",vault,"8M"],check=True)
for p in (QMP,SERSOCK,SERLOG):
    try: os.remove(p)
    except OSError: pass
qemu=subprocess.Popen(["qemu-system-x86_64",*accel_args(),"-machine","q35","-m","512M",
 "-drive","if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
 "-drive",f"if=pflash,format=raw,file={B}/OVMF_VARS6.fd","-drive",f"format=raw,file=fat:rw:{ESP}",
 "-drive",f"if=none,id=zeosvault,file={vault},format=raw","-device","nvme,drive=zeosvault,serial=ZEOSVAULT",
 "-device","virtio-net-pci,netdev=n0","-netdev","user,id=n0,ipv6=on",
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
t0=time.time();ready=False;dhcp=False
while time.time()-t0<75:
    s=ser()
    if "graphical desktop shell up" in s: ready=True
    if "DHCP: bound" in s: dhcp=True
    if ready and dhcp: break
    time.sleep(0.5)
print("desktop:",ready,"dhcp:",dhcp,flush=True)
time.sleep(3.0)   # let ipv6_service tick a few RS + absorb RA
sw("\n");time.sleep(0.6)
sw("ping 10.0.2.2\n");time.sleep(4.0)         # v4 regression check
sw("ip6\n");time.sleep(2.0)                    # show v6 addrs (if command exists)
sw("ping6 fec0::2\n");time.sleep(5.0)          # v6 gateway
sw("ping6 fec0::3\n");time.sleep(5.0)          # v6 DNS
print("=== OUTPUT ===")
print(ser()[-1400:])
_stop[0]=True;qmp("quit")
try: qemu.wait(timeout=5)
except Exception: qemu.kill()
