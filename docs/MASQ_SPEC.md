# MasQ — Temporal Wayfinding Service

**Every piece of state carries proof of who it's been.**

*Codex Labs LLC — 2026*

---

## What MasQ Is

MasQ is the identity and provenance layer of TRISA OS. Every module, dependency, file, and piece of state in the system has a MasQ — an image-based historical record of its address history, name history, and temporal lineage.

MasQ is not a changelog. It's not a Git log. It's not metadata someone typed. It's an unfalsifiable record of where a thing has been, what it was called, and how it got here.

**MasQ gives the system recall.** Not logs. Recall.

---

## Core Properties

### Image-Based Records

MasQ snapshots are images — faithful captures of state, not descriptions of state. You can't edit a MasQ history the way you can rebase a Git log. The history IS the identity.

### Address History

Every piece of state lives at a CFA address. When it moves, the MasQ records both the old and new address. The complete address lineage is navigable.

### Name History

Modules get renamed. Packages get forked. Dependencies change hands. The MasQ carries the full name lineage. Nothing can hide where it came from.

### Temporal Navigation

MasQ isn't a log you search. It's a map you navigate. You move through a module's history the way you move through physical space — by going somewhere. Forward. Backward. To a branch point. To a known-good state.

---

## What MasQ Kills

### Typosquatting
Malicious package `reqeusts` has no MasQ history. It appeared yesterday. The address record is empty. The system sees it immediately — not through a scanner, but through the file's own provenance being thin.

### Dependency Confusion
A malicious package with the same name as an internal one appears on a public registry. The MasQ address history doesn't match. It's never lived where it claims to live.

### Supply Chain Hijack
A maintainer's account is compromised. A poisoned version is published. The MasQ shows the new version's origin doesn't match the historical address pattern. The lineage broke.

### Name Squatting
The MasQ carries full name history. Renamed packages don't lose their past. Forked projects carry their ancestry visibly. You can't hide provenance.

---

## MasQ as Version Control

Traditional version control: "check out commit abc123."

MasQ: "show me where this module lived on Thursday, what it was connected to, what was flowing through it, and what it was called."

You're not reading a flat list of diffs. You're orienting yourself in the temporal map of the system. Wayfinding.

### Navigation, Not Operations

Rollback isn't an operation. It's navigation. You're standing at a point in the MasQ timeline and you move to a different point. The system reconstitutes around that position because VAULT holds the temporal state and CFA provides the addressing.

You don't undo anything. You go somewhere.

Forward movement — deploying, upgrading, experimenting — is also navigation. You're exploring a branch of the temporal map. If you don't like where it leads, you walk back to the junction.

### Debugging as Wayfinding

A signal chain broke. Walk the MasQ backwards. Find when the path changed. The MasQ shows you the fork in the road — not what a developer wrote in a commit message, but what actually happened to the state.

---

## MasQ as Dependency Manager

### Dependency Blocks

Dependencies are scoped to the signal chain that uses them, not the project.

Module A declares its dependency block. Module B declares a completely different one. They coexist because they're .zdx modules with self-contained dependency contracts. They don't share a global tree. They can't conflict.

### Temporal Dependency State

Every dependency block is versioned through time via MasQ. Rolling back isn't "reinstall the old version and pray." It's "load the dependency state from Tuesday." The entire graph at that moment. Intact. Tested. Known-good.

### Parallel Comparison

Preview a new dependency block. Run it in parallel against the current one. Diff the output. If the signal chain produces the same results, swap it live. If it doesn't, you see exactly where the delta is.

### Fork and Snap Back

Fork a dependency block. Experiment. Snap back. Not branching in Git. Not creating a virtual environment. Just "run this module against this alternate dependency set and show me what changes." Instant. Native. Because the OS thinks in deltas, the diff between dependency configurations is just another temporal comparison.

---

## MasQ in Z+

```zplus
// Recall a module's full history
masq.recall("detect.zdx") -> history_view

// Navigate to a previous state
masq.goto("detect.zdx", when: "2026-03-15T14:00:00")

// Diff two points in time
masq.diff("detect.zdx", from: "tuesday", to: "now") -> changes

// Inspect dependency provenance
masq.deps("detect.zdx") -> deps_view

// Require provenance for loaded modules
node secure_detect {
    mde: "yolo-v8.zdx"
    require_masq: true
    min_history: 30d
}
```

---

## Relationship to Other Components

| Component | MasQ's Role |
|-----------|------------|
| **VAULT** | MasQ records are stored in VAULT. Temporal state persistence. |
| **CFA** | MasQ tracks address history within CFA's fractal address space. |
| **MDE** | MDE modules carry MasQ provenance. Hot-swap is MasQ-navigated. |
| **Zixel** | Zixel provides proprioception. MasQ provides recall. Together: a machine with a body and a memory. |
| **NTS** | NTS corrects. MasQ remembers what was corrected and when. |
| **Z+** | MasQ is accessible as a native language feature, not a library. |

---

*MasQ is the system's memory. Not storage. Memory. The difference is recall.*

**Codex Labs LLC — 2026**
