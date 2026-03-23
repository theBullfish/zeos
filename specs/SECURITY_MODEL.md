# Zeos Security Model — Signal Chain Security

> Security is not a feature. It's the shape of the signal graph.
> If a signal can't reach a node, the attack can't happen.
> No permission dialog. No sandbox escape. No path = no access.
>
> **Date**: March 23, 2026
> **Status**: Architecture specification — PARAMOUNT
> **Applies to**: Kernel, Browser, All applications

---

## Core Principle

In conventional OS security, everything is allowed by default and
you add restrictions. In Zeos, nothing is connected by default
and you add wires. Security is the absence of a connection.

**A process that has no wire to the network cannot access the network.**
Not "is denied access." Has no concept that the network exists.
The signal graph IS the permission system.

---

## Threat Model

### What We Defend Against
1. **Remote code execution** — malicious input that becomes computation
2. **Privilege escalation** — signal reaching a node it shouldn't
3. **Data exfiltration** — signal leaving a boundary it shouldn't
4. **Side channels** — timing, power, EM leakage
5. **Supply chain** — untrusted code/modules running in the graph
6. **Physical access** — stolen hardware, cold boot, DMA attacks
7. **Network attacks** — MITM, DNS poisoning, TLS downgrade
8. **Kid safety** — this OS runs in schools; content filtering matters

### What We Don't Defend Against (Yet)
- State-level adversaries with custom hardware
- Quantum computing attacks on current crypto
- Physical destruction of hardware

---

## Layer 1: CFA (Codex Fractal Addressing)

Every memory address is derived from a device-local secret.
There is no flat address space. You cannot guess another
process's addresses because they're seeded differently.

```
address = fractal_derive(device_secret, process_seed, offset)
```

**What this prevents:**
- Buffer overflow exploitation (addresses are unpredictable)
- Return-oriented programming (gadget addresses unknown)
- Heap spraying (allocation layout is process-unique)
- Cold boot attacks (extracted memory is scrambled without the seed)

**What this requires (CRITICAL GAP):**
- Mathematical specification of the fractal derivation
- Hardware seed storage (TPM, fuse-based, or Zixel-derived)
- Page table integration (x86 REQUIRES page tables in long mode)
- Performance: derivation must be < 10ns per access

---

## Layer 2: Signal Graph Isolation

Processes don't share memory. They share NOTHING by default.
Communication happens through explicit signal chains that the
kernel mediates.

```
// Process A can talk to Process B only if wired:
process_a.output -> kernel.mediate -> process_b.input

// The kernel sees every signal crossing a process boundary.
// No signal crosses without a declared wire.
// No wire exists without explicit creation.
```

**What this prevents:**
- Unauthorized IPC (no wire = no communication)
- Confused deputy attacks (kernel validates every crossing)
- Capability leaks (wires are not transferable by default)

**Implementation:**
- Every inter-process edge goes through a kernel gate
- The kernel gate checks: is this wire authorized?
- Wire authorization is declared at process creation
- Wires can be revoked at any time (live disconnection)

---

## Layer 3: Gate-Based Access Control

Gates are not just flow control. They're security boundaries.
A gate can check identity, validate data, enforce rate limits.

```
// File access: only if the process has a wire to the file
process.file_request -> gate(
    authorized: vault.check_access(process, file),
    rate_limit: 100 per second,
    audit: true
) -> vault.read(file)

// Network access: only if the process has a wire to the network
process.net_request -> gate(
    authorized: net.check_policy(process, destination),
    content_filter: csp_rules,
    audit: true
) -> net.send
```

**What this prevents:**
- Unauthorized file access
- Unauthorized network access
- Resource exhaustion (rate-limited gates)
- Unaudited access (every gate can log)

---

## Layer 4: MasQ Provenance

Every module (.zdx), every Z+ program, every data file has
MasQ provenance — a cryptographically verifiable history of
what it is, where it came from, and how it changed.

```
// Before loading a module, check its history
module -> masq.verify -> {
    gate(provenance: valid)   -> load,
    gate(provenance: invalid) -> reject -> alert,
    gate(provenance: unknown) -> quarantine -> user_prompt
}
```

**What this prevents:**
- Trojaned modules (provenance mismatch detected)
- Supply chain attacks (history must be continuous)
- Tampered data (MasQ hash chain is append-only)

**For the browser specifically:**
- TLS certificates verified against MasQ provenance
- Downloaded files quarantined until provenance checked
- JavaScript (or Z+ scripts) from untrusted origins sandboxed

---

## Layer 5: Zixel Hardware Integrity

The machine monitors its own physical state. Anomalies in
timing, temperature, or power indicate attack or failure.

```
// DMA attack detection
zixel.pcie_timing -> gate(anomaly: true) -> {
    pcie.isolate(device),
    alert("PCIe timing anomaly on device {id} — possible DMA attack")
}

// Timing side-channel mitigation
zixel.core_timing -> gate(variance > threshold) -> {
    add_noise(constant_time: true),
    alert("Timing variance detected — adding jitter")
}

// Thermal attack detection (fault injection)
zixel.thermal -> gate(spike > 20C in 100ms) -> {
    halt_sensitive_ops,
    alert("Thermal spike — possible fault injection")
}
```

**What this prevents:**
- DMA attacks (detected via timing anomaly)
- Timing side channels (Zixel detects variance)
- Fault injection (thermal spikes detected)
- Hardware tampering (EMI/power anomalies)

---

## Layer 6: Browser Security

The browser is the most attacked surface. Every protection matters.

### Same-Origin Policy (via Signal Isolation)
Each origin (scheme + host + port) runs in its own signal chain.
No wires between chains = no cross-origin access.

```
// page from a.com cannot read data from b.com
// because they're in separate signal chains with no wire between them
origin_a : signal_chain("https://a.com")
origin_b : signal_chain("https://b.com")
// No edge between origin_a and origin_b = isolation
```

### Content Security Policy
CSP headers become gate rules:

```
response.headers.csp -> csp_rules

// Script loading gated by CSP
dom.scripts -> gate(src: csp_rules.script_src) -> {
    gate(pass)  -> execute,
    gate(block) -> {drop, report(csp_violation)}
}
```

### TLS Enforcement
```
url -> {
    gate(scheme: "http") -> {
        // Upgrade to HTTPS if available
        try_https(url) -> {
            gate(success) -> https_url,
            gate(fail) -> warn_user -> {
                gate(user_accepts) -> proceed_http,
                gate(user_rejects) -> cancel
            }
        }
    },
    gate(scheme: "https") -> verify_cert -> {
        gate(valid) -> proceed,
        gate(expired) -> warn_user,
        gate(revoked) -> block,
        gate(self_signed) -> block_unless_dev_mode
    }
}
```

### Cookie Security
Cookies are VAULT entries scoped to origin:

```
// Set cookie = store in VAULT under origin scope
set_cookie -> vault.store(
    path: "cookies/{origin}/{name}",
    value: cookie_value,
    scope: origin,          // CFA-isolated
    ttl: cookie_expiry,
    flags: {httponly, secure, samesite}
)

// Read cookie = VAULT read with scope check
cookie_request -> gate(
    same_origin: true,
    httponly_check: !from_script,
    secure_check: scheme == "https"
) -> vault.read("cookies/{origin}/{name}")
```

### XSS Prevention
User input never becomes executable without passing through a
sanitization gate:

```
user_input -> sanitize_gate -> {
    gate(contains_script) -> escape_html -> safe_text,
    gate(clean)           -> safe_text
}
safe_text -> dom.insert
// Raw user input has NO wire to dom.insert. Only safe_text does.
```

### Sandboxed Rendering
Each tab/page is a separate signal chain with declared capabilities:

```
page : sandbox(capabilities: {
    network: [same_origin],     // can only fetch from same origin
    storage: [cookies, local],  // can access cookies and localStorage
    media: [images, fonts],     // can load images and fonts
    compute: [limited_cpu],     // CPU time budget
    memory: [256MB]             // memory cap
})
```

---

## Layer 7: Kid Safety (School Environment)

### Content Filtering
```
// School mode: content filter gate on all fetched content
response.body -> content_filter : gate {
    toxicity @ mde("content-safety.zdx") -> gate(< 0.3) -> pass,
    adult    @ mde("adult-detect.zdx")   -> gate(< 0.1) -> pass,
    violence @ mde("violence-detect.zdx") -> gate(< 0.2) -> pass,
    otherwise -> block -> display_block_page
}
```

### Usage Tracking (Transparent)
```
// Students can SEE what's being tracked — no hidden surveillance
url -> vault.append("usage/{student}/{date}.log")
url ~> student_dashboard("sites_visited")

// The student sees their own dashboard. The teacher sees aggregate.
// No hidden tracking. Honest materials.
```

### Time Limits
```
// Configurable per-student session limits
session_timer : gate(elapsed < time_limit) -> allow_browsing
session_timer -> gate(elapsed >= time_limit) -> {
    display("Session time reached. Save your work."),
    delay(5m) -> session.end
}
```

---

## Implementation Priority

1. **Signal graph isolation** — no wire = no access (kernel)
2. **Gate-based access control** — gates validate every boundary crossing
3. **TLS in the browser** — no HTTP by default
4. **Same-origin isolation** — each origin in its own chain
5. **CSP enforcement** — gates from response headers
6. **CFA mathematical spec** — the foundation for everything
7. **MasQ provenance** — trust chain for modules and data
8. **Zixel anomaly detection** — hardware-level intrusion detection
9. **Content filtering** — school/kid safety
10. **Cookie security** — VAULT-scoped, CFA-isolated

---

## Comparison

| Attack | Conventional Defense | Zeos Defense |
|--------|---------------------|-------------|
| Buffer overflow | ASLR, DEP, canaries | CFA (addresses unknowable) |
| XSS | CSP headers, sanitization | No wire from input to DOM |
| Cross-origin data theft | Same-origin policy (JS enforced) | Separate signal chains (structural) |
| Privilege escalation | Capabilities, seccomp | No wire = no concept of the resource |
| Supply chain | Code signing | MasQ provenance (continuous history) |
| Side channel timing | Constant-time code | Zixel detects variance, adds jitter |
| DMA attack | IOMMU | Zixel PCIe timing anomaly detection |
| Session hijacking | HttpOnly, Secure flags | VAULT-scoped, CFA-isolated cookies |

The difference: conventional security is **deny by default with exceptions.**
Zeos security is **disconnected by default with explicit wires.**
Deny can be bypassed. Disconnection cannot — there's nothing to bypass.

**Codex Labs LLC — 2026**
