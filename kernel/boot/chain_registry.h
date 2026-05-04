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

#endif /* ZEOS_CHAIN_REGISTRY_H */
