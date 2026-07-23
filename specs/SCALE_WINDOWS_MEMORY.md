# Zeos — Windows & Memory at Scale

**Status:** Architecture (the scale layer). Not Alpha. Built right, in list order, in
small correct steps — nothing here is a placeholder to be redone in a later version.

**The bar:** a window never loses its content and never blanks. The machine never
freezes or beachballs, no matter how many windows are open. Memory stays flat and
predictable. Switching to any window is instant. The mechanism is invisible — the user
only ever sees "it just works, fast, always."

---

## 1. Why the big three are the wrong model to copy

macOS (WindowServer/IOSurface), Windows (DWM), and Wayland compositors all **bolt**
compositing + memory-compression onto a fundamentally linear OS, then manage memory
**reactively**: per-window backing store, GPU compositing so occluded windows are cheap,
damage tracking, memory compression before disk swap, purgeable buffers — and when the
working set exceeds RAM, they swap, compress, and ultimately OOM-kill. The failure mode
is a **cliff**: fine, fine, fine, then beachball / thrash / kill.

We copy the *good primitives* (backing stores, damage, occlusion, compression) and reject
the *reactive posture*. Zeos manages memory **predictively**, from the same delta loop as
everything else — it watches its own memory and reclaims **before** the wall, so there is
no cliff.

## 2. The Zeos model: a window IS a chain

A window is not a bag of pixels the WM babysits. It is a **signal chain that PRODUCES its
content**. That single fact is the whole advantage:

- **Content is re-derivable.** A window's pixels are the output of resolving its chain. So
  a cold window's backing store is *reclaimable* — drop or compress it, and when the window
  is needed again its chain re-produces the pixels. State that must persist (geometry,
  title, z, focus, the chain graph itself) is tiny and always resident.
- **Redraw is scheduling, not a WM special case.** "Which windows repaint this frame" is
  the chain-resolution scheduler deciding which chains resolve — by visibility, priority,
  and available bandwidth. A minimized or fully-occluded window's chain simply does not
  resolve. No separate "is this window dirty" bookkeeping bolted on the side.
- **"Too many windows + memory pressure" is not a window-manager problem.** It is a
  resource-allocation problem the scheduler already owns.

## 3. The three invariants (the delta thesis, applied to the screen)

1. **Damage is the delta.** Recompute and repaint *only the region that changed*. A window
   that didn't change costs zero. The compositor never redraws the whole screen because one
   pixel moved. (Zeos map: B.5 dirty-region tracking + B.9 partial redraw.)
2. **Occlusion is prediction.** The compositor *knows* what is actually visible. It pays
   only for visible, damaged pixels. Everything offscreen or fully covered is free to draw
   (skipped) and free to reclaim (backing dropped/compressed).
3. **Reclaim is correction.** Under memory pressure the system compresses or drops the
   backing of cold windows and re-derives on demand via the chain. Driven by the scheduler's
   own bandwidth/visibility signal, *ahead* of the wall — predictive, not the OOM-killer's
   reactive panic.

## 4. Compositor architecture (the concrete build)

- **Double buffer / atomic present (B.6).** Compose the frame into a back buffer, then flip
  (or copy the damaged span) to the scanned-out front buffer in one shot. The scanout/reader
  never observes a half-drawn frame — no tearing, no "windows vanish" mid-composite. This is
  the *correct* fix for the present-atomicity class of bug, not a symptom patch.
- **Damage-rect compositing (B.5/B.9).** A per-frame damage set drives the composite. Only
  damaged, visible spans are recomposited: `desktop∩damage → windows∩damage (front-to-back,
  occlusion-culled) → chrome∩damage`. Clip every primitive to the damage rect.
- **Per-window backing store.** Each window's chain renders into its own surface (owned by
  the surface, sized to the window). The compositor blits the visible+damaged region of each
  surface. Moving/occluding a window is cheap (blit + damage the exposed region); its content
  isn't re-rendered unless the content itself changed.

## 5. Memory model

- **Working set = resident.** Visible and recently-used windows keep their backing store hot.
- **Cold = reclaimable.** Occluded/minimized/background windows: backing compressed (fast
  in-RAM codec) or dropped entirely; the chain re-produces on next exposure. Persisted
  per-window state is bytes, not megabytes.
- **Pressure = bandwidth allocation.** The reclaim policy is fed by the scheduler's own view
  of what's live. As free memory falls, cold backings are reclaimed proactively — the system
  is watching its own memory delta ("the processor watches itself think"), so it degrades
  gracefully instead of hitting a cliff.
- **No fixed cap.** `WM_MAX_SURFACES = 32` (today's static array) is removed. Window count is
  bounded by memory + reclaim, not a magic constant.

## 6. Success criteria (verified, not asserted)

- Open N windows (N ≫ 32); every one retains correct content; none blank.
- Fully cover a window, uncover it → pixels correct, instantly, no re-render flash.
- Drive memory to pressure → cold backings reclaim; foreground stays fluid; no freeze/kill.
- Steady-state RSS flat with idle windows (damage=0 ⇒ zero composite work, zero re-render).
- A single composite is never observed half-drawn (atomic present).

## 7. Build order (list order — each complete + verified before the next)

1. **B.6 — double buffer / atomic present.** Foundation; everything composites into it.
2. **B.5 + B.9 — damage tracking + partial redraw.** Clip the composite to damage; occlusion-cull.
3. **Per-window backing stores.** Surfaces own their buffers; compositor blits damaged spans.
4. **Occlusion culling + skip-when-clean.** Idle windows and covered windows cost nothing.
5. **Predictive reclaim.** Compress/drop cold backings under the scheduler's pressure signal;
   re-derive via chain on exposure.
6. **Remove the 32-surface cap.** Dynamic surface set bounded by memory.

No step ships partial. If a step needs a lower layer to be correct, that layer is built as
part of the step — not deferred.
