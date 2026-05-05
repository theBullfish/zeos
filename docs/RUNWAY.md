# Runway

Brad's "follow that list all the way to the end" — committed work order
after the SMP audit lands. No questions, no compromise. Strike each as it
lands; don't add. End is the end.

## Status legend
- `·` queued
- `~` in flight
- `✓` done

## In flight right now
(none)

## Recently completed
- ✓ Per-driver SMP audit (#97 → 7c25027). Honest scope: per-driver locks live; every chain currently affinity-pinned to BSP because transitive paths (DHCP UDP, ACPI battery, etc.) aren't yet swept. AP partition runs but no chain qualifies. Real unlock = per-chain transitive sweep.
- ✓ SMP unblock pass 1 (2026-05-03). Submit-staging locks landed for net_chain / block_chain / mde_chain / hda — these protect the module-static request slot across stage+resolve, complementing the per-chain try-lock that already covered the node walk. CHAIN_MDE lifted to affinity=-1; boots clean -smp 4 with selftest line reporting `RESOLVING — 1/N chains lifted`. CHAIN_NET_TX, CHAIN_BLOCK, CHAIN_AUDIO have their locks LANDED but the lift is GATED — first-boot -smp 4 wedges shortly after AP-side resolves of those chains begin, and the failure is not in the per-driver or staging layer. Needs a follow-up bisect into the AP scheduler / chain framework. CHAIN_NET_RX not attempted; RX deliver path (arp_cache / dhcp_state / tcp_conns / dns_cache) hasn't been swept. SMP stress tests added: smp.stress.{net.tx,block.write,mde,audio} (each SKIPs when its chain is gated).

## The list

1. · TLB shootdown — paging changes propagate across cores via IPI
1b. · Per-chain transitive-path sweep — lift chains off BSP affinity one at a time (net_tx, net_rx, block, mde, audio first)
2. · NVIDIA Stage 2 — locate GSP firmware, embed, load, register Ampere compute backend (#95)
3. · AML EC region — laptop EC battery values via OperationRegion(EC)
4. · Window snap — half/quadrant snapping at the WM layer
5. · Virtual desktops — workspace separation via existing chain hierarchy
6. · Calculator app — programmer + scientific modes, standalone
7. · Text editor — Zeos-shaped: a typed-text chain emitting `text_edit` signals. Spellcheck, autocomplete, autosave, version history are all subscriber chains. UI is a renderer over the chain. Not "an editor app."
8. · File manager — Zeos-shaped: a chain over `fs_event` signals that masq_journal already records. Trash-GC, search index, sync, undo all subscribe. UI is a stream renderer. Not "an app with menus."
9. · Calendar/clock app — uses tod, alarm, world clocks. CHAIN_CLOCK at ~1Hz.
10. · Activity Monitor — Zeos-shaped: a chain that subscribes to all chains via MDE wildcard route. Free integration with notifications + B3 anomaly detection.
11. · Firewall — CHAIN_FIREWALL gating CHAIN_NET_TX/RX with rule table.
12. · Disk encryption — CFA-native: keys are CFA SOVEREIGN handles, blocks are CFA-addressed, MasQ records every encrypted-region access. LUKS is the wrong primitive.
13. · CFA identity contexts (formerly "multi-user accounts") — each context is a CFA root with its own MasQ tier + chain visibility set. Login transitions the active root. Same end-user feel, Zeos primitive.
14. · USB UVC webcams — iso transfers in xHCI + CHAIN_VIDEO_IN.
15. · Print chain (formerly "print spooler") — CHAIN_PRINT pipeline `print_request → format → tx_to_printer → completion`. IPP for wire.

## Zeos-back checkpoints

Every 3 runway items, before launching the next: stop and audit.

For each recent landing:
- Is it shaped like a chain over typed signals, or like an imperative module?
- Does it use CFA / MasQ / chains / Z+ / B3 / MDE primitives, or Linux primitives wearing Zeos labels?
- Are state changes flowing through chain_resolve so vault_version / masq_journal / B3 capture them?
- Is sensitive state CFA-wrapped at the right tier?

For each upcoming item in the runway:
- Re-read the brief I'd give an agent.
- Spot Linux-shape language ("daemon", "spooler", "process", "users with UIDs").
- Reshape the brief BEFORE launching, not after the agent ships drift.

Track checkpoints inline: `## Zeos-back N (after items X-Y)` — record what was caught + corrected.

## Zeos-back checkpoints — log

### Zeos-back 1 (after items 1, 1b, 2, 3, 4, 5, 6)
Caught and corrected before further launches:
- #7 Text editor → chain-over-`text_edit` signals; subscriber chains for spell/autocomplete/autosave/version
- #8 File manager → chain-over-`fs_event` signals; masq_journal is already the source of truth
- #10 Activity Monitor → chain subscribing to all chains via MDE wildcard
- #13 Multi-user → CFA identity contexts (re-named, re-shaped)
- #15 Spooler → CHAIN_PRINT

(#12 disk encryption was a separate catch — CFA-native, not LUKS — landed at b332860.)

## End state

When this list closes, Zeos is:
- Genuinely multi-core (item in flight)
- Multi-vendor compute (CPU + Goya + NVIDIA after #2)
- Real laptop-power-aware (after #3)
- Window-managed at desktop standard (after #4-5)
- Application-layer real (after #6-10)
- Defensively networked (after #11)
- Privacy-baseline complete (after #12-13)
- Multimedia-input capable (after #14)
- Print-output capable (after #15)

After end-of-list: we re-anchor with Brad. Held items (Intel iGPU, AMD,
S3 wake trampoline) stay held until hardware exists. Vendor GPU / weeks-
of-work / specialized items get re-assessed at that point.
