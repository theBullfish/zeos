#!/usr/bin/env python3
# Boot-to-desktop harness for the BUILD_MAP verification sweep.
# Drives cold-boot PIN enrollment + the 5-screen first-boot wizard (arrow/enter),
# reaches the live desktop, and captures a battery of interaction screenshots.
# Headless via QMP. Screens saved to /tmp/zeos-s-*.png, serial to /tmp/zeos-s-serial.txt
import json, os, socket, subprocess, sys, time
from zqemu import accel_args

K=os.path.dirname(os.path.abspath(__file__)); B=os.path.join(K,"build"); ESP=os.path.join(B,"esp")
QMP="/tmp/zeos-s-qmp.sock"; SER="/tmp/zeos-s-serial.txt"
os.makedirs(os.path.join(ESP,"EFI","BOOT"),exist_ok=True)
subprocess.run(["cp",os.path.join(B,"BOOTZ.EFI"),os.path.join(ESP,"EFI","BOOT","BOOTX64.EFI")],check=True)
open(os.path.join(ESP,"startup.nsh"),"w").write("FS0:\r\nEFI\\BOOT\\BOOTX64.EFI\r\n")  # deterministic auto-launch (no UEFI-shell drop)
subprocess.run(["cp","/usr/share/OVMF/OVMF_VARS_4M.fd",os.path.join(B,"OVMF_VARS.fd")],check=True)
vault=os.path.join(B,"vault.img")
subprocess.run(["rm","-f",vault]); subprocess.run(["qemu-img","create","-f","raw",vault,"8M"],check=True)
for p in (QMP,SER):
    try: os.remove(p)
    except OSError: pass

qemu=subprocess.Popen([
    "qemu-system-x86_64", *accel_args(), "-machine","q35","-m","512M",
    "-drive","if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
    "-drive",f"if=pflash,format=raw,file={B}/OVMF_VARS.fd",
    "-drive",f"format=raw,file=fat:rw:{ESP}",
    "-drive",f"if=none,id=zeosvault,file={vault},format=raw",
    "-device","nvme,drive=zeosvault,serial=ZEOSVAULT",
    "-device","qemu-xhci,id=xhci","-device","usb-mouse",  # PS/2 kbd (i8042) drives keys; USB mouse for rel/btn. (usb-tablet abs is IGNORED by Zeos input stack — verified cycle 3.)
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
def ser():
    try: return open(SER,"r",errors="replace").read()
    except OSError: return ""
def shot(tag):
    o=f"/tmp/zeos-s-{tag}.png"; cmd("screendump",filename=o,format="png")
    print(f"[{tag}] {os.path.getsize(o) if os.path.exists(o) else 0} bytes", flush=True)
def key(q):
    cmd("input-send-event",events=[{"type":"key","data":{"down":True,"key":{"type":"qcode","data":q}}}])
    cmd("input-send-event",events=[{"type":"key","data":{"down":False,"key":{"type":"qcode","data":q}}}])
    time.sleep(0.25)
def rel(dx,dy):
    while dx or dy:
        sx=max(-100,min(100,dx));sy=max(-100,min(100,dy));dx-=sx;dy-=sy
        cmd("input-send-event",events=[{"type":"rel","data":{"axis":"x","value":sx}},{"type":"rel","data":{"axis":"y","value":sy}}]); time.sleep(0.03)
def btn(down):
    cmd("input-send-event",events=[{"type":"btn","data":{"button":"left","down":down}}]); time.sleep(0.1)

# 1. Wait for PIN enrollment gate
t0=time.time(); gate=False
while time.time()-t0<40:
    if "PIN enrollment" in ser(): gate=True; break
    time.sleep(0.3)
print("PIN gate:", gate, flush=True)
time.sleep(1.0)
# 2. Enroll PIN 1234 (twice: set + confirm)
for _ in range(2):
    for d in ("1","2","3","4"): key(d)
    key("ret"); time.sleep(0.8)
shot("after-pin")
# 3. Probe the first-boot wizard: screenshot, press enter, repeat.
for i in range(6):
    time.sleep(1.2)
    shot(f"wiz-{i}")
    key("ret")
# 4. Wait for the live desktop (settled oracle: key on the definitive marker,
#    not a blind sleep). Keep nudging enter in case a wizard screen lingers.
t0=time.time(); ready=False
while time.time()-t0<50:
    s=ser()
    if "graphical desktop shell up" in s or "chain-resolution main loop" in s:
        ready=True; break
    key("ret")           # advance any lingering wizard screen
    time.sleep(0.6)
print("desktop shell up:", ready, flush=True)
time.sleep(3.0)
shot("desktop")

# E.7: set-1 scancode -> ASCII. Type a UNIQUE marker (plain keys, no modifiers)
# into the live shell; each char echoes to serial via shell_pump_char->kputc.
# Observable = the marker appears in-order in the serial log (the Terminal window
# itself is a static font_draw mockup, so serial is the real echo channel).
for ch in list("kbtest7"):
    key(ch)
time.sleep(0.4); key("ret"); time.sleep(0.6)

# ── Interaction battery (each screenshot is an evidence artifact) ──
# Cursor motion + click feedback (E.2/E.3)
rel(-200,-150); shot("cursor-moved")
btn(True); time.sleep(0.15); btn(False); shot("cursor-click")
# Command palette via Super+Space (E.8 keybinds, J.3 search palette)
def combo(mods, k):
    # Inter-event settles: firing modifier-down, key, and releases back-to-back
    # occasionally races a heavy composite/persist tick and the combo is dropped
    # (~1/5 boots the overlay never opened). Small gaps make the make/break pair
    # land deterministically without changing what's tested.
    for m in mods:
        cmd("input-send-event",events=[{"type":"key","data":{"down":True,"key":{"type":"qcode","data":m}}}]); time.sleep(0.05)
    cmd("input-send-event",events=[{"type":"key","data":{"down":True,"key":{"type":"qcode","data":k}}}]); time.sleep(0.06)
    cmd("input-send-event",events=[{"type":"key","data":{"down":False,"key":{"type":"qcode","data":k}}}]); time.sleep(0.05)
    for m in reversed(mods):
        cmd("input-send-event",events=[{"type":"key","data":{"down":False,"key":{"type":"qcode","data":m}}}]); time.sleep(0.03)
    time.sleep(0.4)

# K.2: sigviz node-select via KNOWN-POSITION rel (no corner-anchor -> no hot-corner
# conflict). Cursor is at (760,390) here = init center (960,540) + cursor-moved
# rel(-200,-150). memory:0 node center ~ (163,210): rel(-597,-180). 1st click ->
# select (border 1->3px + highlight), 2nd same node -> inspector, empty -> deselect.
combo(["meta_l"],"g"); time.sleep(0.6); shot("k2-open")
rel(-597,-180)
btn(True); time.sleep(0.1); btn(False); time.sleep(0.45); shot("k2-select")
btn(True); time.sleep(0.1); btn(False); time.sleep(0.45); shot("k2-inspector")
rel(500,300); btn(True); time.sleep(0.1); btn(False); time.sleep(0.45); shot("k2-deselect")
key("esc"); time.sleep(0.3)
# Window-management keybinds FIRST (before the palette, which is modal and eats keys).
# Each action captured from a CLEAN base: workspace switch first (Super+F2 -> empty ws,
# Super+F1 -> back), then snap (Super+2), then maximize/restore.
combo(["meta_l"], "f2");   time.sleep(0.6); shot("workspace-2")
combo(["meta_l"], "f1");   time.sleep(0.6); shot("workspace-1")
combo(["meta_l"], "2");    time.sleep(0.6); shot("snap-tr")
combo(["meta_l"], "up");   time.sleep(0.6); shot("maximize")
combo(["meta_l"], "down"); time.sleep(0.6); shot("restore")
# Signal-graph overlay (K.1): Super+G opens. K.4: '=' zooms in (100%->150%), arrows pan.
# (K.2 click-to-select node NOT drivable here: Zeos ignores usb-tablet abs, and relative
#  positioning + sigviz-open are both eaten by INPUT.DROP.COMPOSITE — needs A.6 fix first.)
combo(["meta_l"], "g");   time.sleep(0.6); shot("sigviz")
key("equal"); time.sleep(0.25); key("equal"); time.sleep(0.4); shot("sigviz-zoom")  # K.4 zoom in
key("left");  time.sleep(0.4); shot("sigviz-pan")                                    # K.4 pan
key("esc"); time.sleep(0.3)
# Show-desktop (peek): Super+D
combo(["meta_l"], "d");   time.sleep(0.6); shot("show-desktop"); combo(["meta_l"], "d"); time.sleep(0.3)
# Palette LAST (modal): Super+Space
combo(["meta_l"], "spc"); time.sleep(0.6); shot("palette"); key("esc"); time.sleep(0.3)
# E.5 hot corners: slam the cursor into each 8px corner, dwell >150ms. Corner
# actions print un-gated [HC] serial markers + visible effects. rel() clamps at
# edges so corner positioning is exact. Move toward center between corners to
# exit the zone (resets the triggered flag).
def hc_corner(dx,dy,tag):
    rel(600,400); time.sleep(0.2)   # exit any corner zone (reset)
    rel(dx,dy);   time.sleep(0.4)   # slam to target corner + dwell >150ms
    shot(tag)
hc_corner(-4000,-4000,"hc-tl"); key("esc"); time.sleep(0.3)  # TL -> palette
hc_corner(-4000, 4000,"hc-bl")                                 # BL -> workspace next
hc_corner( 4000, 4000,"hc-br")                                 # BR -> show desktop
hc_corner( 4000,-4000,"hc-tr")                                 # TR -> notifications

cmd("quit")
try: qemu.wait(timeout=5)
except Exception: qemu.kill()
print("--- serial tail ---")
print(ser()[-1200:])
