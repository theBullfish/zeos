#!/usr/bin/env python3
# I.2/I.3/I.5/I.6 verification: boot net-enabled to the desktop, drive the
# `browse` shell command over the serial socket to fetch + render a real page,
# then QMP-screendump the browser window so the on-screen render can be
# inspected. Also drives a scroll + a link-region click for I.5/I.6.
import json, os, socket, subprocess, sys, threading, time
from zqemu import accel_args
K=os.path.dirname(os.path.abspath(__file__)); B=os.path.join(K,"build"); ESP=os.path.join(B,"esp")
QMP="/tmp/zeos-b-qmp.sock"; SERSOCK="/tmp/zeos-b-ser.sock"; SERLOG="/tmp/zeos-b-serial.txt"
URL=os.environ.get("ZURL","http://example.com/")

# tiny host HTTP server so the render is deterministic + offline-safe: a page
# with an <h1>, a paragraph, and a link (exercises CSS defaults + block layout
# + link hit-test).
PAGEPORT=8091
PAGE=(b"<html><head><title>Zeos Render Test</title></head><body>"
      b"<h1>Zeos Browser</h1>"
      b"<p>The quick brown fox jumps over the lazy dog. "
      b"This paragraph exists to exercise block layout and TTF text wrapping "
      b"across multiple lines inside the browser content viewport.</p>"
      b"<p>Another block below with a <a href=\"/next\">link to follow</a> here.</p>"
      b"<hr>"
      b"<p>Footer line after a horizontal rule.</p>"
      b"</body></html>")
def httpd():
    srv=socket.socket(socket.AF_INET,socket.SOCK_STREAM); srv.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
    srv.bind(("0.0.0.0",PAGEPORT)); srv.listen(4)
    while True:
        try: c,_=srv.accept()
        except OSError: break
        try:
            req=c.recv(2048)
            path=b"/"
            if req.startswith(b"GET "): path=req.split(b" ",2)[1]
            print("HTTPD: served",path,flush=True)
            body=PAGE
            c.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %d\r\nConnection: close\r\n\r\n"%len(body)+body)
        except Exception as e: print("HTTPD err",repr(e),flush=True)
        finally:
            try: c.close()
            except: pass
threading.Thread(target=httpd,daemon=True).start()
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
 "-device","virtio-net-pci,netdev=n0","-netdev","user,id=n0","-vga","std",
 "-device","qemu-xhci,id=xhci","-device","usb-tablet,bus=xhci.0",
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
    o=f"/tmp/zeos-b-{tag}.png"; qmp("screendump",filename=o,format="png")
    print(f"SHOT[{tag}] {os.path.getsize(o) if os.path.exists(o) else 0} bytes",flush=True)
def key(q):
    qmp("input-send-event",events=[{"type":"key","data":{"down":True,"key":{"type":"qcode","data":q}}}])
    qmp("input-send-event",events=[{"type":"key","data":{"down":False,"key":{"type":"qcode","data":q}}}]);time.sleep(0.25)
def click(sx,sy):
    ax=int(sx*32767/1920); ay=int(sy*32767/1080)
    qmp("input-send-event",events=[{"type":"abs","data":{"axis":"x","value":ax}},
                                   {"type":"abs","data":{"axis":"y","value":ay}}]);time.sleep(0.3)
    qmp("input-send-event",events=[{"type":"btn","data":{"down":True,"button":"left"}}]);time.sleep(0.15)
    qmp("input-send-event",events=[{"type":"btn","data":{"down":False,"button":"left"}}]);time.sleep(0.5)
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
time.sleep(1.5)
sw("\n");time.sleep(0.8)
sw("browse test:home\n");time.sleep(4.0)
shot("home")
# I.5: hit-test the "Follow this link" anchor via the browser's own click path
# (screen coords from the home shot; the compositor mouse route isn't wired yet).
CX=int(os.environ.get("CLICKX","165")); CY=int(os.environ.get("CLICKY","300"))
sw("bclick %d %d\n"%(CX,CY));time.sleep(3.0)
shot("clicked")
print("=== BROWSE serial ===")
print(ser().split("browse test:home")[-1][-800:])
_stop[0]=True;qmp("quit")
try: qemu.wait(timeout=5)
except Exception: qemu.kill()
