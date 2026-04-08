# Z-OS Alpha 0.1 — "It Is"

> **Date:** April 7, 2026
> **Status:** Architecture locked. Build in progress.
> **Primitive:** The chain (MDE + MasQ + CFA + VAULT)
> **Decision engine:** B3 (Brad's Bayesian Balance)
> **Voice:** Z+ (chain definitions, config, app logic)
> **Muscle:** C (kernel runtime, hardware interface)

---

## The Primitive: Chain

Everything in Z-OS is a chain. A chain is one thing with four aspects:

```
struct chain {
    // MDE — what it does
    chain_node_t  *nodes;        // Processing nodes in this chain
    route_t       *routes;       // How this chain connects to others
    fusion_t      *fusions;      // Merged capabilities with peer chains

    // MasQ — what it perceives
    masq_tier_t    tier;         // Sovereign / Internal / Reference
    perception_t  *can_see;     // Which chains this chain perceives
    // If it can't see it, it doesn't exist. Not denied. Absent.

    // CFA — what it is
    cfa_addr_t     addr;         // Derived from graph position, parent, tier, birth time
    // Address IS identity. Not assigned. Derived.

    // VAULT — what it remembers
    vault_state_t *temporal;     // State across time. Versioned. Persistent.
    // Chains don't save. They remember.

    // B3 — what it believes
    b3_state_t     belief;       // Beta(α, β) — prior, evidence, posterior
    // Every chain has a belief about its own performance.
    // Updated with every resolution. Converges on truth.

    // Runtime
    chain_status_t status;       // Live / Paused / Error / Detached
    uint64_t       last_resolve; // TSC of last resolution
    float          resolve_time; // How long last resolution took
};
```

This replaces: processes, threads, file descriptors, device handles, window handles,
socket descriptors, IPC channels, permission objects, service units, and mount points.

One struct. One primitive. One language to describe it (Z+). One engine to run it (MDE).

---

## Boot Sequence: Chain Zero

```
Power on
  → Chain zero exists (the CPU)
  → Chain zero discovers memory (PMM chain)
  → Memory chain discovers PCI bus (PCI chain)
  → PCI chain discovers devices → each becomes a chain:
      GPU chain (framebuffer output)
      NIC chain (network I/O)
      USB chain (input devices)
      Storage chain (VAULT persistence)
  → MDE builds the route graph from discovered chains
  → B3 initializes: empty priors, learning starts
  → MasQ tiers assigned intrinsically (hardware = Internal, user = Reference)
  → CFA addresses derived from graph topology
  → Compositor chain starts (master bus — mixes visual output chains)
  → Panel chain starts (meters — shows chain health)
  → Desktop chain starts (monitor bus — wallpaper + icons)
  → Input chain starts (UEFI keyboard + mouse → focused surface)
  → First boot? → First boot wizard chain runs (5 screens)
  → Shell chain opens with Z+ REPL
  → Ready.
```

No init system. No runlevels. No service manager. Chains discover chains.

---

## Z+ Chain Definitions

Z+ is how you speak to Z-OS. Chain definitions are Z+:

```zplus
// A kid writes this in Zeros
chain robot_arm {
  sense → think → move
}

// A developer writes this in DereZ
chain web_fetch {
  url → dns_resolve → tcp_connect → tls_handshake → http_get → parse → render
}

// The kernel defines this (same language)
chain compositor {
  surface_1.output → mix
  surface_2.output → mix
  panel.output → mix
  cursor.output → mix
  mix → framebuffer
}
```

Same syntax. Same engine. Same rules. A 10-year-old's chain and the kernel's
compositor are the same primitive.

---

## MDE Routing

Chains don't call each other by name. MDE routes by **type matching**:

```
chain keyboard {
  output: input_event
}

chain text_editor {
  input: input_event
  output: text_buffer
}
```

MDE sees keyboard outputs `input_event`, text_editor accepts `input_event`.
Route established. No hardcoded wiring. No configuration. Types match, signal flows.

When a second keyboard appears (USB hotplug), MDE routes it too. Same type, same route.
When the text_editor closes, the route dissolves. No cleanup code. No dangling handles.

---

## MasQ Perception

Security is not a layer. It's the fabric.

```
chain vault_private {
  tier: sovereign
  // Only sovereign chains can perceive this
}

chain browser {
  tier: reference
  // Cannot perceive sovereign chains
  // Not denied — they literally don't exist from browser's perspective
}
```

A browser chain trying to read sovereign VAULT data doesn't get "permission denied."
It gets nothing. The chain isn't there. There's nothing to hack because there's
nothing to find.

This replaces: ACLs, SELinux, AppArmor, capabilities, sandboxing, containers.

---

## B3 Decision Engine

Every chain has a `b3_state_t`:

```c
typedef struct {
    float alpha;    // Successes + prior
    float beta;     // Failures + prior
    int   n;        // Total observations
} b3_state_t;
```

When MDE needs to choose between two routes for the same signal type:

```
GPU chain A: Beta(47, 3)  → E[f] = 0.94  (fast, reliable)
GPU chain B: Beta(12, 8)  → E[f] = 0.60  (slower, less certain)
```

MDE routes to A. Not because it was configured to. Because B3 learned it.

First boot: both chains have Beta(1, 1) — uniform, no preference. MDE tries both.
After 50 resolutions, the posteriors diverge. The system learned which GPU is better
for this workload. No benchmarks. No configuration. Experience.

---

## CFA Addressing

Chain addresses are fractal. Derived from graph position:

```
/hw/pci/0000:01:00.0/gpu          → GPU chain
/hw/pci/0000:01:00.0/gpu/render   → GPU render subchain
/usr/shell/browser                 → User's browser chain
/usr/shell/browser/dns             → Browser's DNS subchain
```

The address IS the identity. The address IS the location in the graph.
The address contains the parent's address. Zoom in: more detail. Zoom out: still valid.

CFA addresses are sealed (TPM when available). You can't forge an address.
You can't claim to be a chain you're not. Identity is structural.

---

## Three Versions, One OS

| Version | Persona | MasQ Tier | What You See |
|---------|---------|-----------|-------------|
| **Zeros** | Teal | Limited perception | `sense → think → move`. Build robots. Make things. |
| **DereZ** | Magenta | Expanded perception | Full Z+ syntax. Signal inspector. Chain debugging. |
| **Zeos** | Steel blue | Full perception | Everything. MDE routing tables. B3 posteriors. MasQ tiers. Hardware chains. |

Same OS. Same kernel. Same chains. The curtain opens wider. Nothing was hidden.
The kid was always on the real thing.

---

## Ecosystem Integration

Alpha 0.1 connects to the existing repos:

| Repo | Role in Z-OS |
|------|-------------|
| **MDE** | Routing/fusion engine → THE kernel runtime |
| **TRISA** | Preprocessing chain node (signal scoring) |
| **Goya fleet** | Compute chain nodes over TB4 |
| **CFA-lib** | Addressing library (Rust, called from C) |
| **PCB Forge** | Runs as Z+ chain definition |
| **CDE** | Stream decomposition chain nodes |
| **NLC Mapper** | Cognitive mapping chain, connects to VAULT |
| **BLUE Observer** | Collector chains, Z-OS IS the conductor |
| **B3** | Math in repo, engine in kernel |
| **ZAuth** | Identity IS CFA. Address IS auth. |

---

## Alpha 0.1 Deliverables

1. Boots on QEMU and CN60
2. Hardware discovery creates chain graph
3. B3 engine running with Beta distribution updates
4. MDE routing by type matching
5. MasQ perception tiers enforced structurally
6. CFA addresses derived from graph topology
7. VAULT persists chain state across reboot
8. Compositor chain renders desktop
9. Panel shows running chains + health
10. Dock auto-hides, shows pinned + running
11. Window controls L/R, snap/tile, workspaces
12. Z+ REPL — user types chain definitions, they run
13. Signal visualizer shows live chain graph
14. Browser chain loads HTTP pages
15. DHCP → DNS → TCP → HTTP networking
16. Spring physics on all motion
17. Three personas (Zeros / DereZ / Zeos)
18. First boot wizard (5 screens, passive education)
19. Inspectable — tap any chain, see state
20. Rock solid. No stubs. If it's there, it works.

---

## What Alpha 0.1 is NOT

- Feature-complete (no JS, no advanced CSS, no image loading)
- Multi-architecture (x86-64 only, ARM is 0.2)
- Production networking (single TCP, no retransmission)
- Polished fonts everywhere (boot font fallback where TTF not embedded)

---

## Architecture Principle

**Before:** An OS that uses chains as a feature.
**After:** Chains that produce an OS as a side effect.

Z-OS doesn't run chains. Z-OS IS chains.
