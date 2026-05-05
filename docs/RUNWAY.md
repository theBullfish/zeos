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
7. · Text editor app — uses ui_undo/dirty/list_states
8. · File manager app — fat32 + trash + drag/drop + multi-pane
9. · Calendar/clock app — uses tod, alarm, world clocks
10. · Activity Monitor — chain/CPU/memory/network/per-process visibility
11. · Firewall — CHAIN_FIREWALL gating CHAIN_NET_TX/RX with rule table
12. · Disk encryption — CFA-native (not LUKS-shaped). Keys are CFA SOVEREIGN handles, blocks are CFA-addressed, MasQ records every encrypted-region access. The Linux shape (LUKS) is the wrong primitive — we already have the right one.
13. · Multi-user accounts — CFA-tier per-user, login flow extends lockscreen
14. · USB UVC webcams — iso transfers in xHCI + new chain
15. · Print spooler — IPP + SMB to network printer

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
