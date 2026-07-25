# Candidate Programs — append-only backlog

Programs we might need, or existing tools worth cloning-our-way-and-better. **Append-only:**
never edit or delete an entry, only add dated blocks below. Saved incrementally as ideas
surface — not just at the end.

**RULE: never build a NEW program without discussing it with Brad first.** This list is the
queue we talk through together, one at a time.

Each entry: `- [ ] NAME — what it is · why we'd want it · which Language Gap(s) it exercises`

---

## 2026 — opened during Stickler Batch 1

- [ ] **config_surface.zp** — a first-class config/env signal source so programs stop
  hardcoding paths/hosts (`/home/z13/...`, `/var/log/...`, ports). Every reviewed program
  needs it. Exercises: a `config("key")` signal source. *Foundational — likely high priority.*
- [ ] **vault_demo.zp** — a focused program that exercises Vault as the emerging first-class
  signal-emitting data layer (on_change, temporal TTL, memory-pressure signal, scan streams),
  to pin the Vault surface that 02/03/04 all lean on. Exercises the whole `vault.*` gap.
- [ ] **net_source.zp** — pins `net.listen` / `net.interface` / `net.connect` / `respond`
  and the round-trip/return-path concept that 03/04/05 assume. Would settle the
  bidirectional-wire question (§03 FINDINGS).
- [ ] **sustained_gate_demo.zp** — smallest program that defines and exercises the temporal
  gate `gate(> x, sustained: t)` and `on_block`, distinguishing them from instantaneous gates
  and `on_silence`. Settles two Language Gaps at once.
- [ ] **terminal_nodes_demo.zp** — pins `drop`/`accept` terminal-node semantics (signals that
  end, not flow) and program-level `priority: reflex`, both raised by 05_firewall.

*(More will be appended as later batches surface needs. Talk through each with Brad before building.)*
