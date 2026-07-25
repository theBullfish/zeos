# Zeos — Paradigm Conformance Audit ("is it actually Zeos" sweep)

**Authored 2026-07-24.** Five parallel agents read the whole kernel against
`docs/PARADIGM_CONVERSION.md` (2026-05-03) — the doc that honestly admitted every
driver was still shaped like a plain Linux driver despite the chain-resolution
scheduler existing above it. This checks how much of that has actually changed,
file by file, against real evidence (grep + read, not filename-guessing).

**Bottom line up front:** the foundation is genuinely real. The conversion is
real where it's happened (HDA, USB video, block I/O, MDE compute — all landed
since May, all with actual resolve pipelines the scheduler walks). But it hasn't
happened uniformly, and the gaps aren't evenly distributed — they cluster
exactly where you'd least want them: the entire networking stack, the file
that owns window state, and one place where a security comment lies about what
the code actually does.

---

## 1. The foundation itself: real, not stubbed

Checked `chain.c`, `chain_registry.c`, `mde.c`, `mde_chain.c`, `b3.c`,
`cfa_handle.c`, `scheduler.c` end to end.

- **chain.c / chain_registry.c** — real registry, real CFA-derived addressing
  per chain, real MasQ tier-gated perception, ~30 subsystems genuinely wired
  with working node pipelines (not placeholder registration).
- **mde.c** — `topo_sort()` is a real DFS with cycle detection; `mde_resolve_all()`
  walks the topo order respecting SMP ownership and per-chain intervals, feeds
  outcomes into B3. This is the same mechanism A.4 already proved catches a
  hung resolve.
- **b3.c** — textbook-correct Beta-Bernoulli conjugate updates. Not a stub.
- **cfa_handle.c** — **honest naming caveat**: this is a capability/access-control
  indirection table (2048 slots, MasQ-tier-gated resolution, generation
  counters), not literal fractal/tree-traversal address computation. `cfa_resolve()`
  does a tier check then returns the same flat pointer that was wrapped. That's a
  legitimate design — it matches the doc's own stated DMA boundary — but "CFA
  involvement" for any file reduces to one checkable fact: does it call
  `cfa_wrap`/`cfa_resolve` around its sensitive data, or not.
- **scheduler.c** — real LAPIC-preemption + watchdog + B3-driven backoff, not a
  polling loop dressed up in chain vocabulary.

**Verdict: solid.** Nothing built on top of this foundation gets to blame the
foundation for being fake.

---

## 2. Networking — worst offender, ~90% unconverted

20 files checked. **Exactly 1 (`net_chain.c`) is a real chain node.**
`net_tls.c` does real CFA work (TLS keys/sessions wrapped, MasQ-tier gated)
without being a chain node. Every other file — ARP, IP, UDP, TCP/TCP6, DNS,
HTTP, NTP, DHCP/DHCPv6, all four NIC drivers, WPA — is exactly what the May
doc described: plain imperative C, flat pointers, zero chain/CFA/VAULT/B3.
They reach the one real chain node only through a legacy compatibility shim
(`net_drv_send`/`net_drv_recv`) whose own comments say it exists so "existing
callers become chain users without touching their code" — i.e. routing, not
conversion. **No VAULT *persistence* (no vault_write/read calls) and zero B3
anywhere in networking.** (Precise: a couple of net files touch the chain
framework's in-struct `vault_version` counter, but none call the actual VAULT
persistence API — the substantive claim holds.)

`net_rtl8188eu.c` is honest about it — its own header comment admits it's
"intentionally NOT wired into CHAIN_NET_TX/RX."

---

## 3. Storage / USB / filesystem — ~68% unconverted, but real progress landed

19 files checked. Four genuine chain-node conversions have landed **since**
the May doc, not decorative:

- **`hda.c`** — the doc's own recommended pilot (§8), completed. Real 4-node
  pipeline, real VAULT persistence of volume state.
- **`usb_uvc.c`** — the most fully-realized example in the tree. 4-node video
  pipeline, per-camera child chains, VAULT provenance.
- **`block_chain.c`** — real 6-node pipeline including a MasQ journal that
  mirrors writes to VAULT.
- **`mde_chain.c`** — delivers exactly what the doc's §6 asked for (MDE as a
  chain node), which the doc had flagged as *not yet done*.

But the raw driver layer underneath all of that — **NVMe, AHCI, all six USB
host-controller/class drivers, the filesystem/partition layer (fat32.c,
gpt.c), and `vault_disk.c` itself** — is still 100% plain imperative code.
**`vault_disk.c` is not a chain node** despite its name suggesting otherwise;
it's a plain persistence module, and worse, `installer.c` wires its disk I/O
straight to raw `nvme_read`/`nvme_write`, **bypassing `block.c`/CHAIN_BLOCK
entirely** — so VAULT's own on-disk persistence doesn't get the journal/MasQ
benefit every other block consumer gets.

`msix.c` does real CFA work protecting the MSI-X interrupt table at
`MASQ_SOVEREIGN` tier — the one clean CFA example in this stack. No file
anywhere in storage/USB CFA-wraps a DMA buffer; even inside the converted
chain nodes, buffer pointers stay flat.

---

## 4. GPU / compute — real chain nodes, inconsistent CFA

- **`gpu_virtio.c`** — real chain node, CFA-wrapped command buffers. Full compliance.
- **`gpu_nvidia.c`** — real chain node, but `cfa_handle.h` is **imported and
  never called** — dead import, not real usage.
- **`gpu_goya.c`** — real chain node per device (multi-instance, matches
  `GOYA_MAX_DEVICES=8`), but **no CFA wrapping of BAR/DMA handles at all**
  (raw pointers throughout), and **no per-chain `affinity` is ever set** for
  Goya chains.

**Correction to tonight's Dom/Sub spec** (`specs/DOM_SUB_CHIPS.md`): §5 claims
THINK/BACKGROUND routing "reuses `chain_t.affinity` + `smp_chain_owner()`'s
existing routing pattern... no new scheduling primitive needed." That
mechanism does not currently exist for Goya chains — no code sets Goya chain
affinity today. The spec's *design* is still right (that's the correct
existing pattern to extend), but it undersold the work: affinity-based
routing needs to be *built* for Goya chains, not just *reused* as stated.
Filed as a correction, not a redesign — see bible-db Q.5.

---

## 5. Desktop / UI — the most consequential single finding

This is nuanced, not a blanket pass/fail. The compositor's unpreemptible-draw
exemption (documented, deliberate — composite must not be LAPIC-preempted
mid-frame, confirmed by this session's own B.7 fix) genuinely does propagate
to the pixel path: `chain_registry.c` shows panel/dock/desktop's resolve
functions were **deliberately** reduced to state-only stubs, drawing owned by
`compositor_mix_resolve` instead, specifically to keep paint order
deterministic. That's real architecture, not laziness.

But three things prove it's not a blanket exemption:

1. **Panel and dock still do real chain-resolved work for their own state**
   (`panel_update`, `dock_update`) even though drawing bypasses the graph.
2. **Desktop's icon-launch (`desktop_double_click`) genuinely calls
   `chain_create()`** — fully independent of the draw exemption.
3. **`inspector.c` and `palette.c` draw entirely through
   `chain_registry_tick()` with zero bypass** — proof the compositor team
   special-cased only the persistent chrome, not "all UI."

**The real gap: `wm.c` has no chain registration at all** — not even the
inert state-only stub panel/dock got. It's the file that owns actual window
state: create, drag, resize, focus, z-order. It's *less* converted than the
compositor that calls it, with **zero stated architectural reason** (unlike
compositor's documented exemption). The most complex, most user-facing piece
of desktop logic is exactly the one place the paradigm's flagship claim
("everything is a chain") fails without an excuse.

`cursor.c`, `hotcorners.c`, `ui_context_menu.c` are also unconverted with no
stated reason, but lower-stakes than `wm.c` owning window state itself.

---

## 6. Crypto / security — mostly real, one genuine security-relevant lie

Checked `cfa_handle.c` (already covered in §1), `crypto_disk.c`, `identity.c`,
`lockscreen.c`, `access.c`, `firewall.c`, `vault.c`, `vault_disk.c`,
`chat_e2ee.c`, `net_tls.c`, plus `main.c`'s boot-flow CFA claims.

**Real, correct CFA usage:** `crypto_disk.c` (master key), `identity.c`
(per-context PIN), `lockscreen.c` (three PIN buffers), `net_tls.c` (TLS
config/sessions), `vault.c` (base buffer + per-inode structs). `access.c`/
`firewall.c` correctly have none — they don't hold sensitive data, so there's
nothing to gap.

**CORRECTION 2026-07-25 (fleet-review finding #7) — `vault.c`'s CFA is real
but its per-file TIER is not enforced:** `vault.c` genuinely CFA-wraps its
buffers, but `inode_ptr()` (vault.c:165) wraps every inode at a hardcoded
`MASQ_INTERNAL`, ignoring `node->tier`, and `vault_read()` (582-610) never
compares `node->tier` against any observer context. The tier set at
create-time (vault.c:472/507) is only serialized/restored, never checked on
read. **Consequence:** a `VAULT_TIER_SOVEREIGN` file is readable by any
INTERNAL observer — the tier tag is a label, not a gate. This is the root
cause that makes chat_e2ee's SOVEREIGN salt (R.1) meaningless, and it applies
to ALL vault consumers, not just chat. Tracked as bible-db R.4-adjacent; the
real fix is to have `inode_ptr`/`vault_read` honor `node->tier` in the
perceive check. This audit's original "vault.c — real, correct CFA usage"
line above is accurate about CFA *wrapping* but should not be read as "vault
tier isolation works" — it does not.

**Headline finding — `chat_e2ee.c`:** its own header comment claims
"non-members can't reproduce the same digest because cross-context perception
of SOVEREIGN blobs is denied by `cfa_resolve`." **That's false.** Zero
`cfa_handle.h` include, zero `cfa_wrap`/`cfa_resolve` calls anywhere in the
file — the only occurrence of "cfa_resolve" in the whole file is inside that
comment. E2EE room keys (`K_room`, `K_tweak`) sit in plain `uint8_t[32]`
fields in a static array, derived directly into those fields and read
directly for AES-XTS setup. **This is a comment asserting a security property
the code does not provide**, on session-encryption key material specifically
— exactly the class of thing that gets trusted by a future reader (or a future
AI session) without re-checking. Tracked as bible-db R.1, state BROKEN.
**REMEDIATED (comment) 2026-07-25, commit 5362f17:** the false comment was
rewritten to honestly document the gap (no CFA, and — per finding #7 above —
no vault tier enforcement either). The underlying CODE gap (keys still
unprotected) remains open under R.1; only the misleading comment is fixed.

**Secondary, lower-severity:** `main.c`'s cold-boot PIN handoff (the exact
path branded "CFA-native disk encryption" in its own comment) briefly holds
the raw PIN in an unwrapped stack buffer (`pin_buf[24]`) between
`lockscreen_pin_copy()` and `crypto_disk_init()`, before the callee itself
does real CFA work. It's zeroed immediately after — deliberate hygiene, not
negligence — but the comment's "CFA-native" framing overstates what happens
in `main.c`'s own frame. Tracked as bible-db R.2, state PARTIAL (real
mitigation exists, just not the mechanism the comment implies).

---

## Prioritized fix list

1. **`chat_e2ee.c` (R.1, BROKEN)** — either wire real CFA wrapping for
   `K_room`/`K_tweak`, or fix the comment to stop claiming a protection that
   isn't there. The comment is the more urgent fix — a false security claim
   in-tree is worse than an honestly-unconverted file.
2. **`wm.c` (R.3, TODO)** — the single highest-value chain-conversion target
   left in the kernel: it owns the most complex, most-touched state in the
   whole UI and has zero paradigm participation. Candidate for a real
   `CHAIN_WM` node covering create/drag/resize/focus/z-order state (not
   drawing — that stays with compositor's documented exemption).
3. **`vault_disk.c` bypass (R.4, TODO)** — `installer.c` wiring VAULT's own
   disk I/O straight to raw `nvme_read`/`nvme_write`, skipping
   `block.c`/CHAIN_BLOCK, means VAULT's persistence doesn't get the
   journal/MasQ benefit everything else routing through CHAIN_BLOCK gets.
4. **Networking (R.5, TODO, large)** — ~90% unconverted is the single biggest
   remaining body of legacy-shaped code in the kernel. Not urgent (it works),
   but it's the largest gap between "what BUILD_MAP.md implies" and "what's
   actually built the native way."
5. **Dom/Sub spec correction (Q.5 amendment)** — affinity-based Goya routing
   needs to be built, not reused as originally stated.

Not urgent, noted for completeness: `gpu_nvidia.c`'s dead CFA import,
`cursor.c`/`hotcorners.c`/`ui_context_menu.c` unconverted with no stated
reason (same category as `wm.c` but lower stakes since they don't own
persistent state).
