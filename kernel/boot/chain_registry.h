/*
 * Zeos -- Chain Registry
 *
 * The bridge between Z-OS subsystems and the chain/MDE system.
 * At boot, registers every subsystem (compositor, panel, dock,
 * desktop, shell, inspector, palette) as chains with typed nodes.
 * MDE auto-routes them by type matching. Each frame, MDE resolves
 * the entire graph in dependency order -- the old drawing code
 * still runs, but it's triggered BY chain resolution, not by
 * direct function calls.
 *
 * This is where standalone modules become a unified signal graph.
 */

#ifndef ZEOS_CHAIN_REGISTRY_H
#define ZEOS_CHAIN_REGISTRY_H

#include <stdint.h>

/* Register all system chains and wire MDE routes.
 * Returns total chain count (hw + system), or -1 on error. */
int chain_registry_init(void);

/* Called every compositor frame -- resolves all chains through MDE.
 * Returns number of resolution errors (0 = clean frame). */
int chain_registry_tick(void);

/* ── System chain IDs (set during init, -1 if not created) ─────── */

extern int CHAIN_CPU;
extern int CHAIN_COMPOSITOR;
extern int CHAIN_PANEL;
extern int CHAIN_DOCK;
extern int CHAIN_DESKTOP;
extern int CHAIN_SHELL;
extern int CHAIN_BROWSER;
extern int CHAIN_INSPECTOR;
extern int CHAIN_PALETTE;
extern int CHAIN_AUDIO;
extern int CHAIN_NET_TX;
extern int CHAIN_NET_RX;
extern int CHAIN_BLOCK;
extern int CHAIN_MDE;
extern int CHAIN_GPU_0;
extern int CHAIN_GPU_0_RENDER;
extern int CHAIN_GPU_0_DISPLAY;
extern int CHAIN_DISPLAY_GOP;
extern int CHAIN_KEYBOARD;
extern int CHAIN_MOUSE;
extern int CHAIN_SERIAL_IN;
extern int CHAIN_POWER;
extern int CHAIN_BATTERY;
/* CHAIN_POWER_INPUT is declared in power_buttons.h */
extern int CHAIN_TRASH_GC;
extern int CHAIN_IDLE;
extern int CHAIN_NOTIFY;
extern int CHAIN_SETTINGS;
extern int CHAIN_BRIGHTNESS;
extern int CHAIN_WIFI;
extern int CHAIN_BLUETOOTH;

/* Lifetime count of bytes the serial chain has decoded into kb_buf.
 * cmd_selftest samples this across a 100ms window to compute drain
 * rate. */
uint32_t chain_serial_in_total(void);

#endif /* ZEOS_CHAIN_REGISTRY_H */
