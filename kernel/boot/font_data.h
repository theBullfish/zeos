/*
 * Zeos -- Embedded TTF font data
 *
 * These symbols are created by objcopy at build time from:
 *   assets/fonts/inter/Inter-Regular.ttf
 *   assets/fonts/jetbrains-mono/JetBrainsMono-Regular.ttf
 *
 * The Makefile converts TTF -> ELF .o via objcopy and renames
 * the auto-generated symbols to these clean names.
 */

#ifndef ZEOS_FONT_DATA_H
#define ZEOS_FONT_DATA_H

#include <stdint.h>

/* Inter Regular */
extern const uint8_t _binary_inter_regular_ttf_start[];
extern const uint8_t _binary_inter_regular_ttf_end[];

/* JetBrains Mono Regular */
extern const uint8_t _binary_jbmono_regular_ttf_start[];
extern const uint8_t _binary_jbmono_regular_ttf_end[];

/* Noto Sans Regular — F.4 fallback tier (glyphs Inter/JBMono lack) */
extern const uint8_t _binary_noto_regular_ttf_start[];
extern const uint8_t _binary_noto_regular_ttf_end[];

#endif /* ZEOS_FONT_DATA_H */
