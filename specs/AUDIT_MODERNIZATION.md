# Zeos — Modernization Audit ("import don't derive" sweep)

**Authored 2026-07-23.** Five parallel agents read the whole codebase against modern
standard practice, hunting the same class of mistake we just found in the framebuffer
(mapped write-back cached when the field-standard is write-combining): places where we
hand-rolled/naive'd something a well-established better way already solves. This is the
prioritized fix roadmap. Each fix is done + verified before it earns a check (BIBLE).

## The load-bearing insight (measured, not assumed)
The ~120ms compositor frame is **store-bound, not compute-bound.** Proof: enabling `-O2`
moved it ~0ms (instruction-level opt doesn't reduce the ~2M pixel *stores*/frame, and each
store is what costs — in QEMU-TCG and on real HW). The only lever that moves it is
**touching fewer pixels.** Two ways: (a) don't repaint unchanged screen → damage-region
tracking (structural, B.5/B.9); (b) don't blend pixels that are then overdrawn → local
wins that change zero visible output. **Caching content does NOT help by itself** — blitting
a cached buffer is the same store count as regenerating it; caching only pays off *with*
damage (skip the blit when undamaged).

---

## DONE (measured + verified)
- **[x] Shadow: blend visible sliver only** (`wm.c:1192`, commit db34b45). Full window-sized
  shadow blend was ~99% overdrawn by the opaque window body. Blend only the L-shaped
  right+bottom sliver. **Composite ~120ms → ~76ms**, zero visible change, STABLE.
- **[x] Framebuffer write-combining + double buffer** (commit f6e43cb) — the lead finding;
  see BUILD_MAP B.7/B.6.

## Tried + rejected
- **`-O2` build** (`Makefile`) — flipped, built clean, booted STABLE, **but composite
  unchanged (store-bound)**. Reverted: broad latent-UB risk across 100+ files with no
  measured benefit here and no full-subsystem test pass. Re-adopt later *with* a real test
  sweep — it still helps non-composite compute (font raster, TCP, crypto, layout).

---

## Tier 1 — CORRECTNESS bugs (highest priority; small fixes)
- **TCP accepts data with no sequence check** `net_tcp.c:906` — no `seg_seq == rcv_nxt` test;
  a duplicate segment is appended twice → **silent stream corruption**; out-of-order data
  lands at the wrong offset. Fix: drop/ACK segments where `seg_seq != conn->ack` before
  copying (~2 lines). *Low / correctness-critical.*
- **TCP ACKs overflowed/undelivered bytes** `net_tcp.c:909` — on full rx_buf, ACK still
  advances for the *full* segment → peer frees bytes we dropped → permanent hole. Plus a
  hardcoded `window=htons(4096)` (`:191`) that never reflects free space. Fix: advertise real
  `rwnd`, ACK only buffered bytes. *Low / correctness.*
- **`calibrate_tsc` hardcodes 1000 Hz** `timer.c:65` (`tsc_freq = elapsed*20`) — ignores the
  actual PIT `hz`; wrong on any other rate, corrupting `frame_dt`, `timer_wait_ms`, SMP
  delays. Fix: use the passed `hz` in the formula; optionally CPUID leaf 0x15. *Trivial / latent correctness.*
- **DNS ignores TTL + fixed txid** `net_dns.c:130,175` — cache never expires (serves stale
  records); query id constant `0x1234`, responses unmatched → spoofable. Fix: store
  `expires = now + ttl`; randomize + verify txid. *Low / correctness + security.*
- **HTTP can't decode chunked / ignores Content-Length** `net_http.c:355` — chunked bodies
  reach the browser with `\r\n`-hex chunk markers embedded; body end only by connection close.
  Fix: de-chunk / honor Content-Length. *Medium / correctness.*
- **IP inbound fragments treated as whole datagrams** `net_ip.c:83` (MF/offset ignored). Latent
  corruption. *Medium / correctness (rare at MSS traffic).*

## Tier 2 — SECURITY
- **AES-XTS runs in software; AES-NI compiled out** `mbedtls_config.h:30` (no `MBEDTLS_AESNI_C`).
  Table-based AES on the SOVEREIGN master key has a data-dependent cache side channel — undercuts
  the whole `cfa_wrap` key-protection model — and is ~5-10× slower on the per-sector disk path.
  Fix: `#define MBEDTLS_AESNI_C` + `MBEDTLS_HAVE_ASM` (SSE already live at runtime). *Medium / security+perf, top security item.*
- **Crypto salt TSC fallback** `crypto_disk.c:154` — low-entropy KDF salt if hw poll fails
  (guarded/warned). *Low.*

## Tier 3 — COMPOSITE perf (the real A.6 fix = damage architecture; preserves visuals)
Per `specs/SCALE_WINDOWS_MEMORY.md`. These need damage tracking to pay off (caching alone is
store-neutral). Ranked structural:
- **Damage-rect clipping in the compositor** `compositor.c:49,227` — today ANY dirty →
  full-screen recomposite of all four heavy passes (their own "Future" comment). Carry the
  `s_dirty_ring` bounds into a clip/scissor; skip non-intersecting layers. *Med-High / biggest structural win — animated frames ~100ms→few ms.*
- **`present()` copy only the damaged span** `fb.c:63` (today full ~8MB copy/frame). *Low-Med (after damage).*
- **Per-window backing store + occlusion/skip-when-clean** `wm.c:1206,1118` — every window
  re-fills content + re-runs its draw callback every frame, occluded or not. Backing buffer +
  per-window dirty flag + front-to-back occlusion cull. *High.*
- **Dock: 10 stacked full-dock blend layers every frame** `dock.c:424` (fake blur) + per-focus
  halos `:369`. Cache the dock surface, invalidate on hover/focus/item change; blit thereafter.
  (Cutting layers would degrade the look — cache instead, per "looks & feel is the product".) *Med.*
- **Wallpaper refilled every frame** `desktop.c:352` (~22ms) — static fill+strip. Only helps
  *with* damage (skip when undamaged); otherwise blit == refill. *Low, paired with damage.*

## Tier 4 — non-composite perf (independently verifiable)
- **`fb_rect_blend` per-pixel `fb_pixel_blend` call** `fb.c:542` — re-checks bounds + re-derives
  constant alpha per pixel. Hoist clip+alpha, inline; row-at-a-time. Speeds every translucent
  surface. *Low.*
- **Solid fills per-pixel scalar stores** `fb.c:258,77` (`fb_rect`/`fb_clear`) — add `fill32`
  (`rep stosl`). *Small.*
- **Flip/`fb_blit` scalar copy** `fb.c:63,497` — use `memcpy`→`rep movs` / streaming stores;
  clip blit rect once then `memcpy` per row. *Small.*
- **`pmm_alloc` hand bit-scan + rescan-from-0** `pmm.c:225` — O(n²) fill. `__builtin_ctzll(~word)`
  + `next_free_hint`. *Trivial, pure win.*
- **Glyph cache O(n) linear lookup** `font.c:86` — hundreds of thousands of compares/frame once
  full. Packed-key hash. *Low.* **font blit per-pixel multiply** `font.c:218` → scanline.
  **`font_measure` rasterizes for width** `font.c:239` → `stbtt_GetCodepointHMetrics`. Glyph
  eviction bulk-drop-half → LRU. `stbtt_sqrt` 20-iter → early-out.
- **AHCI polling-only, single slot, no NCQ/IRQ** `ahci.c:158,190` (NVMe already does it right).
  *Medium.* **NVMe bounce-copies every I/O** `nvme.c:529` → DMA into caller buffer. *Medium.*
- **FAT: no block/FAT cache** `fat32.c:93` — a disk read + malloc/free per cluster hop; O(n)
  dir scans. 1-block FAT cache. *Low.* **crypto rekeys per sector** `crypto_disk.c:606` — hoist
  `setkey` out of the per-LBA loop. *Trivial.*
- **theme AUTO: two 64-bit divides per color getter per frame** `theme_runtime.c:27` → cache
  once/frame. **heap first-fit linear free-list** `heap.c:61` → segregated bins (*med-large*).

## Verified CORRECT — do not touch
Spring integrator (semi-implicit/symplectic Euler, `anim.c:131`); PAT/WC setup (`vmm.c`);
MMIO mapped UC+volatile (`lapic.c`/`smp.c`); alpha-blend `>>8` approximation; PS/2 input
interrupt-driven; NVMe multi-queue+MSI-X; AES-XTS-256 + PBKDF2 choice; mbedTLS for TLS;
procedural icons; the chain-resolution scheduler / delta thesis (the product — out of scope).
