# Zeos device chain contract

Locked 2026-05-03 from the HDA conversion (commit b9520c0).
Every hardware driver in Zeos follows this shape.

## The shape

A device is a **chain** registered under `chain_registry_init()` with:

- A name (`"audio"`, `"net"`, `"block"`, ...)
- A parent chain (`CHAIN_CPU` for hardware that runs on the host)
- A MasQ tier (`MASQ_INTERNAL` is the default for hardware)
- A linear pipeline of typed nodes, each producing the input type of the next

A node declares:

```
chain_add_node(CHAIN_X, "<name>",
               "<input_type>", "<output_type>",
               <resolve_fn>);
```

`<resolve_fn>` has signature `void resolve(chain_node_t *self, void *input, void *output)`.
Per-node state (DMA buffers, hardware register pointers, fixed-point gains,
pending requests) lives on `self->state`.

## Required ports per device class

| Class    | Pipeline                                                              |
|----------|-----------------------------------------------------------------------|
| Audio    | `pcm_source → volume_filter → pin → hardware_dma`                     |
| Net TX   | `frame_request → l2_encap → mac_filter → hardware_dma`                |
| Net RX   | `hardware_dma → mac_filter → l2_decap → frame_delivery`               |
| Block    | `block_request → addressing → masq_journal → hardware_dma`            |
| USB      | `urb_request → endpoint_route → trb_build → hardware_dma`             |
| Display  | `pixel_source → composite → scanout → hardware_dma`                   |

Every pipeline ends in `hardware_dma` whose output type is `tx_completion` —
that's the type Zixel reads to feed the proprioception layer.

## MasQ provenance rule

Any node that mutates device state (volume change, pin reconfig, stream
start/stop, queue create, MAC filter update, write completion) MUST bump
`chain->vault_version`. Read-only resolves (idle pumps, query-only frames)
do not bump.

```c
chain_t *c = chain_get(my_chain_id);
if (c) c->vault_version++;
```

This is the temporal record. Every state change is timestamped via
`chain_resolve()`'s existing TSC capture and tied to a vault_version
the rest of the system can see.

## B3 belief

`chain_resolve()` already updates `b3_alpha` / `b3_beta` based on whether
the resolve hit `CHAIN_ERROR`. Drivers do nothing extra — set output to
indicate failure (caller convention per type) and B3 tracks reliability
automatically.

## Compat shim rule

Old imperative API stays as a one-line forwarder:

```c
int hda_play_pcm(const int16_t *s, int n, int rate) {
    pcm_request req = { s, n, rate };
    g_audio_pending = &req;
    return chain_resolve(CHAIN_AUDIO);
}
```

External callers don't notice. Internal callers migrate to direct
`chain_resolve()` over time. **Never** delete the imperative entry point
mid-flight — it's the bridge while the rest of the tree converts.

## Init order

1. `hda_init()` / `nvme_init()` / etc — bring controllers live (compat path)
2. `chain_registry_init()` — register every controller as a chain
3. `chain_registry_tick()` — runs every frame, resolves all chains in
   dependency order

Hardware bring-up MUST run before chain registration so each chain's
state pointer points at a valid controller.

## What's not yet locked (queued)

- CFA wrapping of `state` pointers (currently raw void*)
- Inter-chain routes (when a NIC RX completion needs to feed a TLS chain,
  routes are declared in `chain_registry_init`, not hand-wired in C)
- Z+ syntax for declaring chains in a .zp file instead of C — the
  registry C code stays as the bootstrap path
