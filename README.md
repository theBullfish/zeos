# Zeos

Zeos is an OS where the kernel main loop is a typed signal graph. Not "an OS with a lot of features." A different primitive. Every state change in the system — a packet sent, a block written, a pixel pushed to a scanout, a key in a key-value store, a message in a chat room — flows through `chain_resolve()` as a typed signal between named nodes. There is no daemon model, no syscall model, no time-slice scheduler. The scheduler is the chain resolver.

Five primitives sit underneath that loop, and no other shipping OS has them combined. **CFA** (Codex Fractal Addressing) replaces flat pointers with tiered handles — SOVEREIGN keys, INTERNAL kernel state, REFERENCE user data — so the boundary between a TLS session key and a render buffer is structural, not convention. **MasQ** records every state change as a versioned journal entry (`vault_version` bumps per resolve), making provenance a property of the kernel rather than of a logging library. **Chains** are the typed pipelines themselves; a NIC is `frame_request → l2_encap → mac_filter → hardware_dma`, an audio device is `pcm_source → volume_filter → pin → hardware_dma`, and a chat room is a chain of the same shape. **MDE** (Modular Decision Engine) auto-routes work across CPU / Goya / NVIDIA backends by reading the chain graph, not a config file. **B3** (Brad's Bayesian Balance) tracks per-chain reliability through alpha/beta posteriors that bump on every resolve outcome, so the kernel itself has a belief model of its own components.

Below: five native Z+ programs that replace conventional software at orders-of-magnitude less code, on the same hardware, with the line counts measured.

---

## The five replacements

Code-line counts (non-blank, non-comment) measured 2026-05-03 from the source files in `programs/`.

### 1. kv-zeos — replaces Redis

- **`programs/kv-zeos.zp` — 90 code lines (113 total). Redis: ~100,000 LOC.**
- Irreducible primitive: a typed-signal store with addressed lookup, expiration, and change events.
- In Z+: `vault.put(k, v)` / `vault.get(k)` are the lookup. They're persisted by default, CFA-addressed, MasQ-recorded. Expiration is a node on the vault chain. Pub/sub is any chain that subscribes to vault writes by key prefix — typed change signals, no separate channel namespace, no AOF, no RDB.
- Why it isn't a trick: Redis carries a separate persistence path, a separate pub/sub path, a single-threaded core, and a network protocol. Zeos already has each of those as kernel primitives, so the program is the wiring, not the implementation.
- Honest gap: per-call vault.put today; Redis-class throughput needs a batched pipeline node and a B-tree index.

### 2. web-zeos — replaces nginx

- **`programs/web-zeos.zp` — 135 code lines (162 total). nginx: ~150,000 LOC.**
- Irreducible primitive: `http_request → route_match → handler → http_response`.
- In Z+: that four-step pipeline is a chain. Routes are chain wiring (`route("/api/...") -> api_handler`). Static files are a default handler reading from `fat32`. TLS 1.3 is already kernel-resident (`net_tls.c`, CFA-wrapped sessions, IPv6 dual-stack). Caching is a subscriber chain on `http_response`. Workers are AP-pinned chains.
- Why it isn't a trick: nginx ships its own config DSL, its own worker IPC, its own caching layer, its own module compile model. Zeos uses kernel chains for all four — the program declares the wiring.
- Honest gap: server-side TCP listen + accept landed (`dff56f5`); load-balancer/upstream-pool node not yet written.

### 3. build-zeos — replaces make / bazel

- **`programs/build-zeos.zp` — 69 code lines (85 total). bazel: ~500,000 LOC; make: ~30,000 LOC.**
- Irreducible primitive: a typed dependency graph that resolves in topo order on input change.
- In Z+: the chain registry **is** a build system. `chain_resolve` runs every chain whose inputs changed. MDE topo-sorts. `masq_journal` records prior_kind/new_kind transitions. B3 tracks per-rule reliability. A "build target" is a chain with `input_type=source_file`, `output_type=artifact`. Inputs change means `vault_version` on the input bumps — no timestamps to lie.
- Why it isn't a trick: the build graph and the runtime graph are the same graph. The "replacement" is a rename and one verb (`cmd.run`).
- Honest gap: `cmd.run(string)` shell-out wired in pass 3 stdlib; sandbox/hermeticity guarantees not yet there.

### 4. notes-zeos — replaces Notion / Obsidian

- **`programs/notes-zeos.zp` — 67 code lines (75 total). Obsidian: ~200,000 LOC core; Notion: closed, large.**
- Irreducible primitive: text + an inverted index of `[[wiki-link]]` references that updates as text changes.
- In Z+: `editor.c` already emits `text_edit` signals (commit `90b32b4`). `file_mgr.c` emits `fs_event` (commit `7128836`). `CHAIN_NOTES` subscribes to both, parses `[[link]]` tokens on every text change in `.md`, updates the backlink index in vault. Search is another subscriber chain. Any program writing markdown updates the index live.
- Why it isn't a trick: Obsidian re-scans because the app and the kernel disagree about what changed. Zeos doesn't have that disagreement — every fs mutation flows through `CHAIN_FS_EVENT`.
- Honest gap: Z+ regex primitive landed; full-text ranking is naive (no BM25 yet).

### 5. chat-zeos — replaces Slack / Discord (single-host core)

- **`programs/chat-zeos.zp` — 137 code lines (153 total). Slack/Discord: closed, very large.**
- Irreducible primitive: an ordered append-only log of typed `chat_message` signals, scoped by visibility, with subscriber chains for delivery.
- In Z+: each room is a chain at `MASQ_INTERNAL` with member CFA addresses listed. Messages append to a vault-persisted ring per the `masq_journal` pattern. Presence is a chain emitting `presence` signals; subscribers see them via MDE auto-route. Search is a subscriber over `chat_message`. Voice/video reuse `CHAIN_AUDIO`, `CHAIN_VIDEO_IN` (UVC, commit `435497c`), `CHAIN_NET_TX`. Cross-context isolation is automatic via CFA identity contexts (commit `78002aa`).
- Why it isn't a trick: a chat server is already most of what a Zeos kernel does — log, scope, fan out. The protocol-and-directory monopoly is what Slack and Discord actually sell.
- Honest gap: federation between two Zeos hosts (TLS-on-top protocol) not yet written. Single-host case is bounded today.

**Total: 498 code lines of Z+ replace roughly a million LOC of conventional software, on the same hardware, with no separate runtime.**

---

## Why this isn't primitive

Linux at ninety commits looked like a kernel demo too. What made it not a demo wasn't the calculator-and-text-editor surface — it was the primitive: a portable monolithic kernel with a process model and a VFS. Zeos at this stage looks like a kernel with apps and five Z+ files. What's underneath is a typed signal graph as the scheduler, fractal-tier addressing as the memory model, an append-only journal of every state change as the I/O model, a Bayesian belief model of the kernel's own chains, and an auto-routing decision engine across heterogeneous compute. None of those are present in Linux, BSD, Darwin, NT, or any seL4 derivative.

"Apps with a calculator and a text editor" is what a user sees. "Every state change flows through a typed chain with provenance, every keypress is a signal, every NIC frame is a chain resolution, every mutated kernel struct bumps a vault version that is itself queryable" is what's actually executing. The five replacements are the demonstration that the primitive is dense enough to absorb whole categories of conventional software at line-count ratios of 1000:1 or worse — they're not the destination.

The destination is: every device class fits one of six chain shapes (audio, net TX, net RX, block, USB, display — see `docs/CHAIN_CONTRACT.md`), every state change is timestamped and provenance-attributed for free, every workload routes itself across silicon for free, and the OS has a belief model of its own reliability that the user can read.

---

## What's in the box right now

- **Boot path:** UEFI → splash → cold-boot lockscreen (PIN over PBKDF2-HMAC-SHA256, 120k iters) → shell. No text scroll on boot.
- **Hardware drivers (host):** Intel e1000, RTL8169, virtio-net (4 NIC drivers); NVMe + AHCI block; xHCI USB with HID + Mass Storage + UVC video classes; HDA audio with codec walk; PS/2 keyboard + mouse; PCI enumeration with vendor DB; CMOS RTC; ACPI tables + AML interpreter (partial); SMBus.
- **Networking:** dual-stack IPv4/IPv6, TLS 1.3 with full Mozilla CA bundle, WPA2, DHCP, DNS, ARP, ICMP, TCP server-side listen+accept landed (`dff56f5`).
- **Compute:** CPU + Goya HL-1000 backend + NVIDIA Stage-1 scanout (Stage-2 firmware staged, GSP load pending). MDE selector picks per-workload across backends.
- **Multi-core:** AP boot via NASM trampoline. TLB shootdown wired (queued in runway). SMP submit-staging locks landed for net / block / mde / hda. CHAIN_MDE lifted to `affinity=-1` and runs unpinned. CHAIN_NET_TX / CHAIN_BLOCK / CHAIN_AUDIO have locks landed but lift gated; CHAIN_NET_RX not yet attempted. **1 of 4 chains lifted off BSP**; the rest run pinned.
- **Security:** CFA SOVEREIGN / INTERNAL / REFERENCE tiers in use across kernel state. CFA identity contexts replace UNIX users (per-context salt, key, MasQ tier, namespace). CFA-native disk encryption: AES-256-XTS keyed by per-context PIN PBKDF2 (commit `b618562`). CHAIN_FIREWALL with stateful conntrack inserted into CHAIN_NET_TX/RX.
- **Apps:** Calculator (programmer / scientific / standard, 3 modes), Calendar/clock with alarms, Editor (5-chain decomposition: emitter + spell + autocomplete + autosave + version), File manager (5-chain: fs_event + trash_react + index + undo + notify), Activity Monitor (CHAIN_SYSTEM_STATE + CHAIN_ACTIVITY_ANOMALY, B3-driven alerts, 600-sample history), Image viewer, plus the five release replacements (kv / web / build / notes / chat) with polished window UIs.
- **Z+ language:** Three completed passes — pass 1 first-class strings + typed structs, pass 2 modules + import, pass 3 stdlib (`time`, `crypto`, `json`, `regex`, `cmd`, `http`, `btree`, `fs`). Nine standard modules. Four bounded gap-closers landed (`2d340f2`).
- **Multi-display:** virtio-gpu driver iterates every PCI function (cap 4), each with its own CHAIN_GPU + per-scanout chains and backing buffer. Per-scanout flush gating so a 144Hz panel doesn't stall a 60Hz panel.

---

## Honest holes

Where Zeos is **not** done. These are not weaknesses we hide.

- **AMD GPU driver:** no hardware in the lab stack; held until then.
- **Intel iGPU driver:** no hardware in the lab stack; held until then.
- **Real S3 wake trampoline:** code path exists, validation requires hardware not yet wired.
- **Per-chain SMP transitive sweep:** 1 of 4 target chains lifted (CHAIN_MDE). Net TX / Block / Audio have submit-staging locks landed but lift wedges on AP-side resolve — needs a follow-up bisect into AP scheduler.
- **JPEG decode in image viewer and UVC preview:** placeholder rendering; no decoder yet.
- **Voice/video federation in chat-zeos:** single-host today; Zeos↔Zeos federation needs a small TLS-on-top protocol that isn't written.
- **AML EC sub-ops:** the AML interpreter doesn't cover the EC OperationRegion variants that some laptop firmwares use for battery; battery values fall back to ACPI table reads where available.
- **NVIDIA Stage 2:** GSP firmware located and staged; RM channel completion pending. Today's NVIDIA support is scanout-only.
- **Server-side load balancer in web-zeos:** upstream-pool node not yet declared.

Compare against `docs/GPU_HOLES.md` for the full per-hole map across discovery, memory, command submission, mode-set, color, planes, and audio.

---

## Building / running

```
tools/zeos-build/zeos build           # produces kernel/build/BOOTZ.EFI
tools/zeos-build/zeos run             # QEMU smoke (single GPU, single scanout)
tools/zeos-build/zeos run-multigpu    # two virtio-vga devices, two scanouts
```

Real-hardware bring-up is target-specific. Lab targets: ASUS CN60 Chromeboxes (USB image flashed, first boot pending). Build artifact today: `kernel/build/BOOTZ.EFI` at ~102 MB (debug). Self-test reports per-subsystem pass/skip including `Design system .... 30 surfaces audited, 38 fixes applied` and `RESOLVING — 1/N chains lifted`.

---

## Reading order

For somebody who wants to understand the primitive, in order:

1. `specs/DESIGN_SYSTEM.md` — visual language and motion physics
2. `docs/CHAIN_CONTRACT.md` — the six device chain shapes and the MasQ provenance rule
3. `docs/PARADIGM_CONVERSION.md` — the arc from POSIX-shaped drivers to native chains
4. `docs/REPLACEMENTS.md` — the first-principles teardown of the five replacements
5. `programs/30_chain_native.zp` — smallest example of declaring a chain in Z+
6. `kernel/boot/chain.h` — the C primitive
7. `kernel/boot/scheduler.c` — the resolver loop

After that, `docs/RUNWAY.md` covers what closed and what is held, and `docs/OS_LITTLE_THINGS.md` covers the texture backlog that comes after the primitive is done.

---

## Visual identity

Zeros = teal. DereZ = magenta. Full = steel blue. Sovereign tints gold. Reference tints blue. No pure black, no pure white. Every motion is a spring.

---

## License & IP

Zeos, NTS, Zixel, CFA, MDE, MasQ, Barca, VAULT, and the Autonomic Silicon Architecture are subject to provisional patent filings by Codex Labs LLC. CFA filed 2026-03-15. See `LICENSE`.

Built by Codex Labs LLC — Minneapolis, MN.
