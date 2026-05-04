/*
 * Zeos -- Intel HD Audio (HDA) driver, public API
 *
 * Chain-native HDA driver. Pipeline:
 *   pcm_source -> volume_filter -> hda_pin -> hardware_dma
 * Each node is a chain_node_t resolved through chain_resolve().
 * MasQ tier: INTERNAL. Parent: CHAIN_CPU.
 *
 * hda_init() does the legacy PCI/CORB/RIRB/codec bringup so the
 * controller is live before chain_registry_init() runs. The chain
 * nodes drive the actual playback path.
 *
 * hda_play_pcm() is a thin compat shim that builds a pcm_request
 * and calls chain_resolve(CHAIN_AUDIO).
 */

#ifndef ZEOS_HDA_H
#define ZEOS_HDA_H

#include <stdint.h>
#include "chain.h"

/* ── Public API ────────────────────────────────────────────────────── */

int         hda_init(void);
int         hda_ready(void);
int         hda_play_pcm(const int16_t *samples, int num_samples, int sample_rate);
const char *hda_status(void);

/* ── Chain node resolves (called by chain_resolve via chain_registry) ─ */

void hda_pcm_source_resolve   (chain_node_t *self, void *input, void *output);
void hda_volume_filter_resolve(chain_node_t *self, void *input, void *output);
void hda_pin_resolve          (chain_node_t *self, void *input, void *output);
void hda_dma_resolve          (chain_node_t *self, void *input, void *output);

/* ── Chain signal types ────────────────────────────────────────────── */

/* "pcm_request" -- a request to play PCM samples. Built by hda_play_pcm
 * (or by the future native API). The request is staged in module-level
 * state because chain_resolve's scratch buffer is small (4 KB) -- only
 * the descriptor flows through the scratch, the bulk samples stay where
 * the caller put them. */
typedef struct {
    const int16_t *samples;
    int            num_samples;   /* total int16 samples (frames * 2 for stereo) */
    int            sample_rate;
    int            valid;         /* 1 = staged, 0 = idle */
    int            error;         /* 0 = ok, set by nodes on failure */
} hda_pcm_request_t;

/* "pcm_frame" -- a PCM buffer in flight through the pipeline. Carries
 * pointer + count plus a back-reference to the request so volume/pin
 * can mark errors. */
typedef struct {
    int16_t *samples;
    int      num_samples;
    int      sample_rate;
    int      error;
} hda_pcm_frame_t;

/* "dma_descriptor" -- a DMA descriptor that hardware_dma will program
 * into SD0. Points at the audio_buf that hda_pin already filled. */
typedef struct {
    uint64_t buf_phys;
    int      bytes;
    int      frames;
    int      sample_rate;
    int      error;
} hda_dma_descriptor_t;

/* "tx_completion" -- result of a stream run. */
typedef struct {
    int      ok;
    int      frames_played;
    uint32_t elapsed_ms;
} hda_tx_completion_t;

#endif /* ZEOS_HDA_H */
