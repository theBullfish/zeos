#!/usr/bin/env python3
# H.6 WebSocket verification: run a host-side WS echo server (reachable from the
# guest at the user-net gateway 10.0.2.2), boot net-enabled, drive `ws` over the
# serial socket, and capture the roundtrip.
import json, os, socket, subprocess, sys, threading, time, hashlib, base64, struct
from zqemu import accel_args
K=os.path.dirname(os.path.abspath(__file__)); B=os.path.join(K,"build"); ESP=os.path.join(B,"esp")
QMP="/tmp/zeos-w-qmp.sock"; SERSOCK="/tmp/zeos-w-ser.sock"; SERLOG="/tmp/zeos-w-serial.txt"
WSPORT=8090
GUID="258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
def ws_echo():
    srv=socket.socket(socket.AF_INET,socket.SOCK_STREAM); srv.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
    srv.bind(("0.0.0.0",WSPORT)); srv.listen(2)
    while True:
        try: c,_=srv.accept()
        except OSError: break
        try:
            data=b""
            while b"\r\n\r\n" not in data:
                d=c.recv(1024)
                if not d: raise IOError()
                data+=d
            key=None
            for line in data.split(b"\r\n"):
                if line.lower().startswith(b"sec-websocket-key:"): key=line.split(b":",1)[1].strip()
            acc=base64.b64encode(hashlib.sha1(key+GUID.encode()).digest())
            c.sendall(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: "+acc+b"\r\n\r\n")
            hdr=c.recv(2)
            if len(hdr)<2: raise IOError()
            masked=hdr[1]&0x80; ln=hdr[1]&0x7F
            if ln==126: ln=struct.unpack(">H",c.recv(2))[0]
            mask=c.recv(4) if masked else b"\x00\x00\x00\x00"
            pl=bytearray()
            while len(pl)<ln: pl+=c.recv(ln-len(pl))
            if masked:
                for i in range(len(pl)): pl[i]^=mask[i%4]
            out=bytearray([0x80|0x1])
            if len(pl)<126: out.append(len(pl))
            else: out+=bytes([126])+struct.pack(">H",len(pl))
            out+=pl; c.sendall(out)
        except Exception: pass
        finally:
            try: c.close()
            except: pass
threading.Thread(target=ws_echo,daemon=True).start()
time.sleep(0.3)

os.makedirs(os.path.join(ESP,"EFI","BOOT"),exist_ok=True)
subprocess.run(["cp",os.path.join(B,"BOOTZ.EFI"),os.path.join(ESP,"EFI","BOOT","BOOTX64.EFI")],check=True)
open(os.path.join(ESP,"startup.nsh"),"w").write("FS0:\r\nEFI\\BOOT\\BOOTX64.EFI\r\n")
subprocess.run(["cp","/usr/share/OVMF/OVMF_VARS_4M.fd",os.path.join(B,"OVMF_VARS.fd")],check=True)
vault=os.path.join(B,"vault.img"); subprocess.run(["rm","-f",vault]); subprocess.run(["qemu-img","create","-f","raw",vault,"8M"],check=True)
for p in (QMP,SERSOCK,SERLOG):
    try: os.remove(p)
    except OSError: pass
qemu=subprocess.Popen(["qemu-system-x86_64",*accel_args(),"-machine","q35","-m","512M",
 "-drive","if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
 "-drive",f"if=pflash,format=raw,file={B}/OVMF_VARS.fd","-drive",f"format=raw,file=fat:rw:{ESP}",
 "-drive",f"if=none,id=zeosvault,file={vault},format=raw","-device","nvme,drive=zeosvault,serial=ZEOSVAULT",
 "-device","virtio-net-pci,netdev=n0","-netdev","user,id=n0",
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
while time.time()-t0<40 and "PIN enrollment" not in ser(): time.sleep(0.3)
time.sleep(1.0)
for _ in range(2):
    for d in "1234": key(d)
    key("ret");time.sleep(0.8)
for _ in range(6): time.sleep(1.0);key("ret")
t0=time.time();ready=False;dhcp=False
while time.time()-t0<70:
    s=ser()
    if "graphical desktop shell up" in s: ready=True
    if "DHCP: bound" in s: dhcp=True
    if ready and dhcp: break
    time.sleep(0.5)
print("desktop:",ready,"dhcp:",dhcp,flush=True)
time.sleep(2.0)
sw("\n");time.sleep(1.0)
sw("ws 10.0.2.2:%d / helloZeos\n"%WSPORT);time.sleep(8.0)
print("=== WS OUTPUT ===")
print(ser().split("ws 10.0.2.2")[-1][-800:])
_stop[0]=True;qmp("quit")
try: qemu.wait(timeout=5)
except Exception: qemu.kill()
