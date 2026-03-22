# Z-OS Quality-of-Service Wins

> **Classification**: Secondary to the Prime architecture (signal chains, CFA, VAULT, MasQ, Chirp).
> These are the mechanical, operational, and QoL improvements that a clean-sheet x86_64 OS
> collects by not carrying 50 years of abstraction layers. None of these bend the architecture.
> The architecture serves itself first; these are the dividends.
>
> **Date**: March 22, 2026
> **Status**: Inventory complete, not yet implemented

---

## Bucket 1: FREE — Problems That Never Form

These cost zero engineering. The signal graph, CFA, and clean-sheet design mean these
problems cannot exist in Z-OS. They are structural absences, not features.

### Audio Stack Collapse
**Legacy pain**: Linux audio traverses OSS → ALSA → PulseAudio → PipeWire → JACK, each layer
adding latency, resampling, and routing indirection. Professional audio still requires
PREEMPT_RT patches. Per-app routing is buried in pavucontrol.

**Z-OS**: Audio is a signal chain. A source node connects to a sink node. The signal graph
handles routing, mixing, and scheduling natively. There is no audio subsystem — there are
signal chains that happen to carry audio. Latency is bounded by the graph's resolution
interval, not by five stacked daemons.

### Display Scaling and Hot-Plug
**Legacy pain**: X11 has a single global DPI. Wayland supports per-monitor scaling but GTK3
can't do fractional, Qt does it differently, and Electron does its own thing. Unplugging a
monitor piles windows on the remaining display with no memory.

**Z-OS**: Applications render into a device-independent coordinate space. The compositor
handles all scaling, color management, and tone mapping. Apps never see physical pixels.
There is no "DPI awareness" flag because there is no legacy assumption that pixels are
physical.

### DNS Resolver Wars
**Legacy pain**: systemd-resolved, NetworkManager, dnsmasq, Avahi, and /etc/resolv.conf all
compete. Upgrading Fedora 42 broke DNS until systemd-resolved was manually restarted. mDNS
and LLMNR have separate conflicting implementations.

**Z-OS**: One resolver. DNS, mDNS, LLMNR, DoT, DoH, captive portal detection — one service,
one configuration surface. There is nothing to conflict with because there is only one.

### Clipboard Persistence
**Legacy pain**: X11 clipboard content vanishes when the source app closes. Wayland improved
this partially. Windows clipboard history is limited and unreliable.

**Z-OS**: VAULT is the storage model. Clipboard is a temporal VAULT entry — typed (text,
image, file reference, structured data), persistent, survives app exit. History is native.

### Driver Crash Isolation
**Legacy pain**: A buggy kernel driver in Linux or Windows crashes the entire system. Ring-0
code with full privileges, no isolation.

**Z-OS**: Drivers are signal contracts in userspace. IOMMU-isolated. A crashed driver's
signal chain terminates and restarts. The kernel continues. Same insight as Fuchsia and
Redox, but native to the signal graph model.

### Update/Reboot Model
**Legacy pain**: Windows forces reboots. Linux requires reboots for kernel updates. Halfway-
through-an-update states corrupt systems.

**Z-OS**: MDE hot-swap is the native runtime model — modules replace while running. For
kernel-level changes, image-based partition flip (write new image to secondary partition,
atomic switch on reboot, instant rollback). No "updating, do not turn off" state.

### IPC Fragmentation
**Legacy pain**: Pipes, sockets, shared memory, D-Bus, Binder, COM, XPC — every OS has
multiple competing IPC mechanisms bolted on over decades.

**Z-OS**: The signal graph IS the IPC. Processes don't exist as isolated address spaces
that need plumbing to communicate — they are nodes in a graph. Data flows along edges.
There is nothing to fragment because there was never a wall to punch through.

### Power Management Tool Sprawl
**Legacy pain**: cpupower, turbostat, auto-cpufreq, power-profiles-daemon, TLP, thinkfan,
nbfc all compete on Linux. They conflict with each other.

**Z-OS**: One thermal policy engine. Sensors, fans, CPU frequency, GPU clocks — all
actuators in one graph. Constraints in, actions out. One interface.

### File Permissions from 1970
**Legacy pain**: rwx/user/group/other from 1970s Unix. ACLs exist but nothing uses them.
SELinux/AppArmor are so complex most users disable them.

**Z-OS**: CFA is capability-based. No ambient authority. Every resource requires an explicit
capability grant. The permission model is the memory model — not a bolt-on.

### Sleep/Wake Driver Chaos
**Legacy pain**: 10,000 drivers each independently save and restore state on sleep/wake.
Any one of them can break resume. Modern Standby drains 5-15% battery/hour on misbehaving
hardware because drivers prevent deep C-states.

**Z-OS**: Signal chains suspend and resume as graph state. The graph checkpoints and
restores atomically. Individual drivers don't independently manage their own sleep state —
the graph does.

### API Splits
**Legacy pain**: XInput vs DirectInput. X11 vs Wayland. Win32 vs UWP. Core Audio vs WASAPI.
Every major subsystem has at least two APIs because one was built to replace the other and
both survive forever.

**Z-OS**: One input API. One display model. One audio path. One network stack. There is no
old API to keep alive because there is no old API.

---

## Bucket 2: CHEAP — Small Work, Huge Payoff

These need deliberate design but are straightforward engineering. Each is a contained
feature, not an architectural change.

### USB Device Identity Persistence
**Legacy pain**: Every unplug/replug or sleep/wake cycle gives USB devices new device nodes.
No persistent identity. udev rules can match serial numbers but many devices don't report
unique serials.

**Z-OS**: Assign each device a stable UUID from (vendor, product, serial, topology path).
If no serial, fingerprint from descriptor set + topology. Persist device settings
(permissions, power, driver binding) across reconnections. The device is a named node in
the signal graph, not an ephemeral /dev entry.

### Spurious Wake-Source Learning
**Legacy pain**: Firmware leaves PCIe ports or USB controllers as spurious wake sources.
Gigabyte motherboards wake immediately from suspend. User fix: hand-edit /proc/acpi/wakeup.
No OS learns from repeated spurious wakes.

**Z-OS**: Track every wake event with source attribution. If a device causes spurious wakes
more than N times in a window, automatically disable it as a wake source and notify the
user. After entering sleep, verify expected time elapsed via hardware timer — if woken in
<5 seconds, suppress the source and re-enter sleep.

### Display Profiles by EDID Serial
**Legacy pain**: Unplugging a monitor loses window layout. KDE and GNOME have partial
"display profile" support but it's fragile.

**Z-OS**: On monitor connect, match by EDID serial number. Restore window layout, color
profile, scaling factor, refresh rate. Hot-unplug stashes positions. Hot-replug restores.
The display is a persistent signal graph node, not an ephemeral output.

### Bluetooth Codec Transparency
**Legacy pain**: PulseAudio silently switches to 8kHz mono HSP/HFP when any app opens a
recording stream. PipeWire is better but the degradation is still invisible to the user.
A2DP is one-directional by Bluetooth SIG spec — can't use mic while streaming.

**Z-OS**: Before switching from A2DP to HFP: "App X wants microphone. This reduces audio
to 16kHz mono. Allow?" The user decides. The signal graph can visualize the codec path.

### Battery Charge Limiting
**Legacy pain**: Supported on ThinkPads via thinkpad_acpi, ASUS via asus-nb-wmi. No standard
interface. Most brands have no Linux support at all.

**Z-OS**: At boot, enumerate EC charge-control interfaces (auto-detect by platform).
Default to 80% limit. "Full charge for travel" button. Track cycle count and capacity
degradation over time. Expose battery health as system telemetry.

### TRIM Verification
**Legacy pain**: Some SSDs have firmware bugs where TRIM corrupts data (WD Green 3D NAND).
The OS trusts the drive's self-report. fstrim.timer runs weekly, which is arbitrary.

**Z-OS**: After issuing TRIM, periodically read back trimmed sectors. Verify they return
zeros or deterministic pattern. Flag drives that return stale data. Log it. Adjust
filesystem behavior if the drive is untrustworthy.

### Drive Trust Scoring
**Legacy pain**: Storage firmware is a black box. Drives can lie about FLUSH completion.
Consumer SSDs lack power-loss protection. 2 of 4 tested NVMe drives lost FLUSHed data on
power cut.

**Z-OS**: Maintain a database of drive firmware versions and known bugs (updateable, like
fwupd/LVFS). Score drives on TRIM, FLUSH, and power-loss trustworthiness. Adjust
journaling aggressiveness per drive. Untrusted drive → more aggressive journaling.
Trusted drive → lighter overhead.

### NVMe Hot-Plug by Default
**Legacy pain**: Requires `pci=pcie_bus_perf` kernel boot argument on Linux. Works on RHEL
but not reliably on Debian. SATA hot-plug needs AHCI mode in firmware.

**Z-OS**: Configure PCIe MPS/MRR for hot-plug compatibility at boot. Detect hot-plug
capable ports. Enable surprise removal handling. No boot argument hacks.

### RTC Is UTC, Period
**Legacy pain**: Windows stores local time in the RTC. Linux stores UTC. Dual-booting causes
time to jump by timezone offset. Known issue for 20+ years.

**Z-OS**: UTC in the RTC. No option for local time. Display conversion in userspace only.
This is a one-line design decision that eliminates an entire class of bugs.

### RTC Drift Modeling
**Legacy pain**: Hardware clocks drift ~20ppm (seconds per day). OS compensates after boot
via NTP. Offline for a week → minutes of drift.

**Z-OS**: Measure RTC drift rate at every boot/shutdown cycle. Maintain a drift model.
When offline, apply correction to produce accurate time without NTP. When online, use
hardware timestamping on PTP-capable NICs for sub-microsecond accuracy.

### Timezone as Live Data
**Legacy pain**: IANA tz database updates multiple times per year (governments change DST
rules). On Linux it's a package update. On Windows it's baked into Windows Update.

**Z-OS**: Timezone data is a live-updateable data file, decoupled from OS release cycle.
Like a browser's root certificate store — pulled independently, applied immediately.

### Hub Reset Staggering
**Legacy pain**: KVM switch changes trigger bus reset. All devices on the hub re-enumerate
simultaneously. Thundering-herd problem.

**Z-OS**: When hub reset detected, stagger device re-enumeration with backoff. On
enumeration failure, retry with increasing delays and voltage stabilization rather than
giving up after 2-3 attempts.

### USB-C PD Visibility
**Legacy pain**: OS has limited visibility into PD negotiation. Users can't answer "why is
my laptop charging at 45W instead of 100W?" UCSI sysfs is empty on many platforms.

**Z-OS**: Surface PD negotiation state (voltage, current, power role, data role) in system
telemetry. Allow user to request role swap or power renegotiation. Expose capabilities of
connected devices.

### Firmware Compatibility Database
**Legacy pain**: fwupd/LVFS is excellent but depends on vendors uploading firmware. Many
vendors only provide Windows update tools.

**Z-OS**: Integrated firmware compatibility database. Live-updated. Tracks firmware versions,
known bugs per platform, ACPI quirks. Used by the ACPI validator, drive trust scorer, and
thermal policy engine.

### UEFI Variable Sandboxing
**Legacy pain**: Writing to UEFI variables bricked Samsung laptops. Lenovo hardware has
capsule coalescing bugs. No standard behavior when variable storage is full.

**Z-OS**: Never write directly to UEFI variable storage. Maintain shadow copy. Batch
writes. Enforce variable count and size budget. The firmware cannot be bricked by the OS
overwriting its variable store.

### Semantic HID Layer
**Legacy pain**: HID descriptors are ambiguous. Hat switches, D-pads, buttons are unlabeled.
SDL provides userspace unification but it's a library, not an OS service. XInput and
DirectInput are separate APIs on Windows.

**Z-OS**: One input API for all devices. On top of raw HID parsing, maintain a crowdsourced
database of device-specific mappings keyed by vendor/product ID. Ship known-good mappings
for top 500 input devices. User overrides shared back to database.

### HDR Compositing by Default
**Legacy pain**: Windows HDR has washed-out SDR content. Mixed HDR/SDR multi-monitor is
broken on all platforms. Wayland color management protocol only merged February 2025.

**Z-OS**: Compositor operates in wide-gamut linear-light color space internally. SDR content
tone-mapped up. HDR content passed through. No "HDR mode" toggle. No mode to toggle.

### Per-Output Variable Refresh Rate
**Legacy pain**: VRR works on single-monitor gaming. Breaks on multi-monitor with different
refresh rates. Compositors still working out per-output VRR.

**Z-OS**: Each output runs its own refresh loop. Compositor submits frames independently per
output. No global vsync. Each display signal chain runs at its own rate.

### Captive Portal State Machine
**Legacy pain**: systemd-resolved docs say to disable DNSSEC during captive portal detection.
Handoff between "behind portal" and "authenticated" is unreliable. Custom DoT/DoH breaks
captive portals entirely.

**Z-OS**: Captive portal is a distinct network state with its own DNS behavior, firewall
rules, and UI. Detect → present portal page → detect auth completion → transition to normal
networking. Single integrated flow. DoT/DoH suspended during portal state, restored after.

### WiFi Roaming with Signal Policy
**Legacy pain**: Laptops cling to -80dBm AP while -30dBm AP is 3 meters away. wpa_supplicant
has bgscan but doesn't automatically roam. 802.11r is supported but routinely broken by AP
firmware.

**Z-OS**: Maintain continuous signal quality map. Roaming policy: "if current AP drops below
-70dBm and a known AP is above -50dBm, initiate fast transition." 802.11r/k/v default when
AP supports it, automatic fallback when it doesn't.

### ACPI Table Validation at Boot
**Legacy pain**: Linux carries hundreds of DSDT quirks. Firmware ships buggy AML bytecode
tested only against Windows. acpi_serialize boot flag exists solely for firmware bugs.

**Z-OS**: Parse all ACPI tables at early boot. Flag known patterns of buggy AML
(unserialized methods, incorrect wakeup sources, impossible thermal zones). Apply automatic
fixes where safe. Report unfixable issues with vendor-specific guidance.

### IPP-Only Printing
**Legacy pain**: IPP, eSCL, WSD, LPD, raw socket, vendor-specific protocols all compete.
Windows 11 24H2 broke eSCL scanner discovery for Canon, HP, Brother, Epson.

**Z-OS**: IPP Everywhere as the only print protocol. Modern printers all support it. For
scanners, eSCL natively in the OS scan service. Driverless operation as default. Drop LPD,
raw socket, WSD, and vendor protocols.

---

## Bucket 3: HARD — Genuine Physics

These are real spec complexity in the hardware. The work doesn't vanish with a clean
design. Honest accounting.

### ACPI AML Interpretation
Firmware ships a Turing-complete bytecode program (AML) that the OS must interpret.
Vendors test against Windows only. Z-OS must carry a quirk table and an AML interpreter.
The ACPI validator (Bucket 2) mitigates but does not eliminate.

### WiFi Firmware Blobs
Vendor binary firmware loaded to the radio. No way around it on commodity hardware.
Intel, Qualcomm, MediaTek, Broadcom — all require blobs. Must ship or download them.

### NVMe Command Set Complexity
47 admin commands. Queue pair negotiation. Namespace management. Command set extensions.
"Signal contracts" is a cleaner framing for the driver interface, but the command
protocol complexity is inherent to the hardware spec.

### USB Enumeration Timing
Hardware-timed protocol: reset → chirp → speed negotiation within specific timing windows.
Must handle USB 1.1 through 4.0. Electrical conditions vary between boots and between
ports. Enumeration failures are electrical, not software.

### Bluetooth SIG Spec Limitations
A2DP is one-directional by specification. Cannot use microphone while streaming A2DP.
HSP/HFP is 8-16kHz mono. LE Audio (LC3 codec, Bluetooth 6.0) improves this but requires
new hardware on both ends. Adoption is slow. Z-OS can handle the transition gracefully
(Bucket 2: codec transparency) but cannot fix the underlying spec.

### Vendor Embedded Controller Firmware
Fan curves, thermal sensors, charge limits — all locked behind vendor-specific EC firmware
with minimal exposed interfaces. Each laptop model is different. ACPI exposes what the EC
vendor decided to expose, which is often very little. Z-OS can enumerate and use what's
available (Bucket 2: EC introspection), but cannot create interfaces the EC doesn't expose.

### Zixel on Commodity x86
Out-of-order execution, branch prediction, DVFS, and speculative execution make timing-
delta environmental sensing extremely difficult on commodity x86 silicon. Proven on
controlled FPGA paths. Unproven on x86. This is a research problem, not an engineering
problem. Requires empirical work on target hardware.

---

## The Ratio

| Bucket | Count | Engineering Cost |
|--------|-------|-----------------|
| FREE (structural absence) | 11 | Zero |
| CHEAP (deliberate, contained) | 22 | Small per item |
| HARD (genuine physics) | 7 | Research + quirk tables |

33 wins for minimal engineering. 7 that need real work. None bend the architecture.

---

## Design Principle

> These are dividends of the Prime architecture, not goals that shape it.
> The signal graph was not designed to fix audio latency. Audio latency does not exist
> because the signal graph does not create the conditions for it.
> If a QoL win requires compromising signal chains, CFA, VAULT, MasQ, or Chirp —
> the QoL win is rejected. The architecture serves itself. The savings follow.
