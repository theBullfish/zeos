/*
 * Zeos -- Intel HD Audio (HDA) driver, public API
 *
 * Minimum-viable HDA: enumerate controller via PCI (class 0x04 sub 0x03),
 * reset, set up CORB/RIRB, walk codecs, find an output pin, configure one
 * output stream (SD0) for 48 kHz / 16-bit stereo, and play PCM samples.
 */

#ifndef ZEOS_HDA_H
#define ZEOS_HDA_H

#include <stdint.h>

int hda_init(void);
int hda_ready(void);
int hda_play_pcm(const int16_t *samples, int num_samples, int sample_rate);
const char *hda_status(void);

#endif /* ZEOS_HDA_H */
