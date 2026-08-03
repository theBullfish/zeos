#!/usr/bin/env python3
# Active-traffic H verification: boot net-enabled to the desktop, then drive the
# real shell net commands (dns / fetch / ping) over a BIDIRECTIONAL serial socket
# and capture their output. Zero kernel-side code — pure verification harness.
import json, os, socket, subprocess, sys, threading, time
from zqemu import accel_args

K=os.path.dirname(os.path.abspath(__file__)); B=os.path.join(K,"build"); ESP=os.path.join(B,"esp")
QMP="/tmp/zeos-h-qmp.sock"; SERSOCK="/tmp/zeos-h-ser.sock"; SERLOG="/tmp/zeos-h-serial.txt"
os.makedirs(os.path.join(ESP,"EFI","BOOT"),exist_ok=True)
subprocess.run(["cp",os.path.join(B,"BOOTZ.EFI"),os.path.join(ESP,"EFI","BOOT","BOOTX64.EFI")],check=True)
open(os.path.join(ESP,"startup.nsh"),"w").write("FS0:\r\nEFI\\BOOT\\BOOTX64.EFI\r\n")  # deterministic auto-launch (no UEFI-shell drop)
subprocess.run(["cp","/usr/share/OVMF/OVMF_VARS_4M.fd",os.path.join(B,"OVMF_VARS.fd")],check=True)
vault=os.path.join(B,"vault.img")
subprocess.run(["rm","-f",vault]); subprocess.run(["qemu-img","create","-f","raw",vault,"8M"],check=True)
for p in (QMP,SERSOCK,SERLOG):
    try: os.remove(p)
    except OSError: pass

qemu=subprocess.Popen([
    "qemu-system-x86_64", *accel_args(), "-machine","q35","-m","512M",
    "-drive","if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
    "-drive",f"if=pflash,format=raw,file={B}/OVMF_VARS.fd",
    "-drive",f"format=raw,file=fat:rw:{ESP}",
    "-drive",f"if=none,id=zeosvault,file={vault},format=raw",
    "-device","nvme,drive=zeosvault,serial=ZEOSVAULT",
    "-device","virtio-net-pci,netdev=n0","-netdev","user,id=n0",
    "-chardev",f"socket,id=ss,path={SERSOCK},server=on,wait=off","-serial","chardev:ss",
    "-display","none","-qmp",f"unix:{QMP},server,nowait",
],stdout=subprocess.DEVNULL,stderr=subprocess.STDOUT)

# ── serial socket: background reader appends to SERLOG; serwrite() sends ──
ser_sock=None; t0=time.time()
while time.time()-t0<20:
    try: ser_sock=socket.socket(socket.AF_UNIX); ser_sock.connect(SERSOCK); break
    except OSError: time.sleep(0.3)
if not ser_sock: print("SER FAIL"); qemu.kill(); sys.exit(1)
_stop=False
def _reader():
    f=open(SERLOG,"ab")
    ser_sock.settimeout(0.5)
    while not _stop:
        try: d=ser_sock.recv(4096)
        except socket.timeout: continue
        except OSError: break
        if not d: break
        f.write(d); f.flush()
threading.Thread(target=_reader,daemon=True).start()
def ser():
    try: return open(SERLOG,"r",errors="replace").read()
    except OSError: return ""
def serwrite(s):
    ser_sock.sendall(s.encode())

# ── QMP for PIN + wizard keyboard ──
qs=None; t0=time.time()
while time.time()-t0<15:
    try: qs=socket.socket(socket.AF_UNIX); qs.connect(QMP); break
    except OSError: time.sleep(0.3)
if not qs: print("QMP FAIL"); qemu.kill(); sys.exit(1)
qf=qs.makefile("rw")
def qmp(e,**a):
    m={"execute":e}
    if a: m["arguments"]=a
    qf.write(json.dumps(m)+"\n"); qf.flush()
    while True:
        l=qf.readline()
        if not l: return None
        o=json.loads(l)
        if "return" in o or "error" in o: return o
qf.readline(); qmp("qmp_capabilities")
def key(q):
    qmp("input-send-event",events=[{"type":"key","data":{"down":True,"key":{"type":"qcode","data":q}}}])
    qmp("input-send-event",events=[{"type":"key","data":{"down":False,"key":{"type":"qcode","data":q}}}])
    time.sleep(0.25)

# 1. PIN enrollment
t0=time.time()
while time.time()-t0<40 and "PIN enrollment" not in ser(): time.sleep(0.3)
time.sleep(1.0)
for _ in range(2):
    for d in ("1","2","3","4"): key(d)
    key("ret"); time.sleep(0.8)
# 2. first-boot wizard (enter through)
for _ in range(6):
    time.sleep(1.0); key("ret")
# 3. wait for desktop + DHCP bound
t0=time.time(); ready=False; dhcp=False
while time.time()-t0<45:
    s=ser()
    if "graphical desktop shell up" in s: ready=True
    if "DHCP: bound" in s: dhcp=True
    if ready and dhcp: break
    time.sleep(0.5)
print("desktop:",ready,"dhcp:",dhcp,flush=True)
time.sleep(2.0)

# 4. drive shell net commands over serial, capture output
mark_len=len(ser())
for cmd in ["\n", "dns example.com\n", "dns google.com\n"]:
    serwrite(cmd); time.sleep(4.0)
serwrite("fetch example.com /\n"); time.sleep(8.0)
serwrite("https example.com /\n"); time.sleep(12.0)

print("=== NET SHELL OUTPUT (after commands) ===")
print(ser()[mark_len:][-2200:])

_stop=True
qmp("quit")
try: qemu.wait(timeout=5)
except Exception: qemu.kill()
