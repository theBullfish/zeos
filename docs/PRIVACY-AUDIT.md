# Zeos — Network & Privacy Audit

**Audited:** 2026-08-11 · **Tree:** `bible/zplus-zir-convergence` · **Method:** source audit
of every outbound-capable call site, plus observed serial output from real cold boots.

**Why this document exists:** Zeos's proof-of-use deployment is **high-school robotics
students** — minors, on school equipment. A claim of "we don't collect anything" is worth
nothing unless it can be checked. This file states exactly what Zeos puts on the network,
what it does not, and where to look to verify it yourself.

If you find anything in the tree that contradicts this document, that is a bug — report it.

---

## 1. What Zeos transmits at boot, with no user action

Exactly two things, both **local-network address autoconfiguration**, both standard on
every operating system:

| # | What | Where | Scope |
|---|------|-------|-------|
| 1 | **DHCP DISCOVER** | `net.c:98` → `dhcp_start()` | LAN broadcast. Asks the local router for an IP address. |
| 2 | **IPv6 Router Solicitation** | `net.c:103` → `ipv6_start()` | Link-local multicast. Standard SLAAC. |

Both are triggered by `net_init()`. Its only real call site is shell startup
(`shell.c:6854`); two further grep hits (`chain_registry.c:855`, `net_ipv6.c:679`) are
**comments**, not calls. Neither
carries any information about the user, the machine's owner, or any file. Neither contacts
any server operated by us or anyone else — they address the local router only.

Observed on a real cold boot:
```
DHCP: discovering (async)...
IPv6: link-local fe80::5054:ff:fe12:3456 (RS sent, SLAAC async)
DHCP: bound 10.0.2.15 gw 10.0.2.2 dns 10.0.2.3
```

**Nothing else leaves the machine at boot.**

## 2. What Zeos does NOT do

Verified by searching the entire source tree:

- **No telemetry, analytics, or phone-home of any kind.** There is no such code to disable.
  Searching `telemetry|analytics|phone_home` returns **3 hits, all comments** using the word
  in an internal-signal sense, none of which transmit: `signal.c:14` ("computation IS
  telemetry" — a design note on Zixel), `dom_sub.h:49` (a scheduling-class comment), and
  `activity.c:1015` (the Activity Monitor's on-screen network view, which has **zero**
  transmit calls and discloses its own limits on screen).
  *Search hygiene note:* grepping for `beacon` also matches **802.11 WiFi beacon frames** in
  `net_rtl8188eu.c/.h` and `chain_registry.c` — that is wireless scanning, an unrelated
  meaning of the word, not tracking.
- **No automatic time sync.** NTP (`net_ntp.c`) has **no caller** outside its own file; it
  runs only when explicitly invoked.
- **No automatic update check.** `updater.c` has no boot-time or timer-driven caller.
- **No auto-connect to wireless networks**, no auto-loaded homepage, no background fetch.
- **The first-boot wizard makes no network calls at all** — setup is fully offline.
- **DNS lookups happen only for addresses the user asks for** (e.g. typing a URL).

## 3. Hardware annotations and the sharing gate

Zeos keeps a local database of what it learned driving real hardware
(`os/boot/hwnotes.c`). This is **local and fully functional offline**; it never needs a
network or a server.

Contribution to a shared database is **opt-in and currently inert**:

- **Default is OFF.** A fresh install shares nothing. The consent value lives at
  `/hw/share_consent`; absent file means never asked, which means off.
- **One choke point.** `hwnote_share_payload()` is the only function in the system that can
  turn annotations into transmittable bytes. It returns `-1` and writes nothing unless
  consent is set. There is no path around it.
- **No upload code exists yet, deliberately.** The consent architecture was built and proven
  before any network path. Any future upload must route through that same function.
- **Informed consent.** The endpoint and the *exact byte-for-byte payload* are displayed
  before the user decides (`hwnote share`).
- **Revocable**, and the choice persists in both directions — it never silently re-defaults.

**What the payload would contain, in full** (this is the entire format):

```
zeos-hwnotes/1
p 17cb:0308 00/00/00 07 xhci Q6A-USB-verified-on-metal
```

Per device: bus type, vendor:device ID, class/subclass/prog-if, flags, the driver protocol
used, and a free-text engineering note. **Hardware facts only.** No user content, no
filenames, no usernames, no serial numbers, no machine or network identifiers, no location,
no timestamps tied to a person.

Observed refusal with consent off:
```
status: sharing is OFF (default -- nothing is transmitted)
payload: REFUSED -- consent not granted, nothing serialized
```

## 4. For schools and guardians

- Zeos is usable **entirely offline**. No account, no registration, no server dependency.
- Student work stays in the local encrypted vault. Nothing syncs anywhere by default.
- Sharing hardware annotations, if ever enabled, should be an **adult/institutional
  decision**, not a student one — and it transmits no student data even then.

## 5. How to verify these claims yourself

```sh
# every file that can transmit
grep -lE "tcp_send|udp_send|http_get|http_post|net_send|tcp_connect" os/boot/*.c

# any telemetry-style code (expect: no output)
grep -rnE "telemetry|analytics|beacon|phone_home" os/ installers/

# who starts network activity at boot
grep -rn "net_init()" os/boot/*.c
grep -n "dhcp_start\|ipv6_start" os/boot/net.c

# NTP / updater callers (expect: none outside their own files)
grep -rn "ntp_sync\|ntp_query\|updater_check" os/boot/*.c

# the single sharing choke point and its refusal
grep -n "hwnote_share_payload" -A6 os/boot/hwnotes.c
```

On a running system: `hwnote share` shows the current state, the endpoint, and the exact
payload — or the refusal.

## 6. Known limits of this audit

Stated plainly so the document isn't over-trusted:

- This is a **source audit plus observed boot output**, not a packet capture on physical
  hardware over an extended run. A wire-level capture on the target device is the stronger
  test and has not been done yet.
- It covers the `os/` and `installers/` trees. Vendored third-party code (mbedTLS, QuickJS,
  lodepng, musl math) is included in the searches above but has not been line-audited.
- Firmware blobs shipped for specific devices (`os/lib/firmware/`) are vendor binaries and
  are not auditable by us; they are loaded into the device, not executed as OS code.
