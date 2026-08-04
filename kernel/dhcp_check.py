import os,socket,subprocess,time,threading
from zqemu import accel_args
K=os.path.dirname(os.path.abspath(__file__)); B=os.path.join(K,"build"); ESP=os.path.join(B,"espd")
SER="/tmp/zeos-dc-ser.txt"
os.makedirs(os.path.join(ESP,"EFI","BOOT"),exist_ok=True)
subprocess.run(["cp","/tmp/zeos-stable/BOOTZ.EFI",os.path.join(ESP,"EFI","BOOT","BOOTX64.EFI")],check=True)
open(os.path.join(ESP,"startup.nsh"),"w").write("FS0:\r\nEFI\\BOOT\\BOOTX64.EFI\r\n")
subprocess.run(["cp","/usr/share/OVMF/OVMF_VARS_4M.fd",os.path.join(B,"OVMF_VARSd.fd")],check=True)
v=os.path.join(B,"vaultd.img");subprocess.run(["rm","-f",v]);subprocess.run(["qemu-img","create","-f","raw",v,"8M"],check=True)
try:os.remove(SER)
except:pass
q=subprocess.Popen(["qemu-system-x86_64",*accel_args(),"-machine","q35","-m","512M",
 "-drive","if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
 "-drive",f"if=pflash,format=raw,file={B}/OVMF_VARSd.fd","-drive",f"format=raw,file=fat:rw:{ESP}",
 "-device","virtio-net-pci,netdev=n0","-netdev","user,id=n0",
 "-serial",f"file:{SER}","-display","none"],stdout=subprocess.DEVNULL,stderr=subprocess.STDOUT)
t0=time.time()
while time.time()-t0<70:
    time.sleep(2)
    try:s=open(SER,errors="replace").read()
    except:s=""
    if "DHCP: bound" in s or "KERNEL PANIC" in s: break
s=open(SER,errors="replace").read() if os.path.exists(SER) else ""
print("DHCP_BOUND:", "DHCP: bound" in s)
print("PANIC:", "KERNEL PANIC" in s)
print("IPV6_UP:", "link-local" in s)
import re
for ln in s.splitlines():
    if "DHCP" in ln or "IPv6" in ln or "PANIC" in ln: print("  |",ln.strip()[:90])
q.kill()
