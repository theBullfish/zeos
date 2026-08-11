/*
 * Zeos -- Signal Chain Visualizer
 *
 * Draws signal chains as graphical diagrams on the framebuffer.
 * Students see data flow. Engineers see timing.
 * Same data, different views. MasQ at the display level.
 *
 * Interactive: click nodes, zoom/pan, live state animation.
 */

#ifndef ZEOS_SIGVIZ_H
#define ZEOS_SIGVIZ_H

#include <stdint.h>

/* Colors for node states -- derived from theme.h design tokens */
#include "theme.h"

#define SIGVIZ_IDLE      COLOR_ON_SURFACE_4   /* quaternary text -- idle */
#define SIGVIZ_READY     COLOR_WARNING        /* amber -- ready to fire */
#define SIGVIZ_RUNNING   COLOR_PRIMARY        /* persona accent -- active */
#define SIGVIZ_DONE      COLOR_SUCCESS        /* teal -- complete */
#define SIGVIZ_ERROR     COLOR_DANGER         /* red -- error */
#define SIGVIZ_EDGE      COLOR_ON_SURFACE_3   /* tertiary text -- edges */
#define SIGVIZ_TEXT       COLOR_ON_SURFACE     /* primary text */
#define SIGVIZ_BG        COLOR_SURFACE        /* background */
#define SIGVIZ_NODE_BG   COLOR_SURFACE_HIGH   /* raised surface -- node fill */
#define SIGVIZ_BORDER    COLOR_SEPARATOR      /* border/divider */

/* Get current persona's accent for node highlights (defined in shell.c) */
uint32_t theme_accent(void);
uint32_t theme_accent_dim(void);

/* ── Zoom / Pan limits ──────────────────────────────────────────── */

#define SIGVIZ_ZOOM_MIN   0.5f
#define SIGVIZ_ZOOM_MAX   3.0f
#define SIGVIZ_ZOOM_STEP  0.25f

/* ── Layout node (computed position for each chain in the graph) ── */

#define SIGVIZ_MAX_LAYOUT 128

typedef struct {
    int chain_id;
    int x, y;           /* computed position (before zoom/pan) */
    int w, h;           /* node dimensions (before zoom) */
    int depth;          /* tree depth (0 = root) */
    int child_index;    /* index among siblings */
    int child_count;    /* number of children */
} sigviz_layout_node_t;

/* ── Visualizer state ───────────────────────────────────────────── */

typedef struct {
    /* View transform */
    float zoom;         /* 0.5 .. 3.0, default 1.0 */
    int   pan_x;        /* pixel offset, default 0 */
    int   pan_y;        /* pixel offset, default 0 */

    /* Selection */
    int   selected_id;  /* chain_id of selected node, -1 = none */

    /* Animation */
    uint32_t frame;     /* monotonic frame counter for pulse/flash */

    /* Flash on resolve: chain_id that just resolved, countdown */
    int   flash_id;
    int   flash_frames; /* frames remaining for resolve flash */

    /* Drawing area (set each draw call) */
    int   area_x, area_y, area_w, area_h;

    /* Computed layout */
    sigviz_layout_node_t layout[SIGVIZ_MAX_LAYOUT];
    int layout_count;
} sigviz_state_t;

/* ── Core API ───────────────────────────────────────────────────── */

/* Initialize visualizer state (call once) */
void sigviz_init(void);

/* Draw a signal chain graph (all chains, tree layout).
 * chain_id: root chain to draw (-1 = draw all roots)
 * x, y: top-left corner on screen
 * w, h: available drawing area
 */
void sigviz_draw(int chain_id, int x, int y, int w, int h);

/* ── Interactive selection ──────────────────────────────────────── */

/* Hit-test: find which chain node is at screen (x, y).
 * Selects the node if found, deselects if clicking empty space.
 * If clicking an already-selected node, opens inspector.
 * Returns the chain_id of the hit node, or -1 if none. */
int sigviz_click(int x, int y);

/* Get the currently selected chain_id, or -1 if none */
int sigviz_get_selected(void);

/* Clear selection */
void sigviz_deselect(void);

/* ── Zoom / Pan ─────────────────────────────────────────────────── */

void sigviz_zoom_in(void);
void sigviz_zoom_out(void);
void sigviz_zoom_set(float level);
void sigviz_pan(int dx, int dy);
void sigviz_pan_reset(void);

/* ── Animation triggers ─────────────────────────────────────────── */

/* Call when a chain finishes resolving (triggers accent flash) */
void sigviz_notify_resolved(int chain_id);

/* Advance frame counter (call once per frame, before draw) */
void sigviz_tick(void);

/* K.1: chain-graph overlay (Super+G). */
void sigviz_overlay_toggle(void);
int  sigviz_overlay_visible(void);
void sigviz_overlay_close(void);
void sigviz_overlay_draw(void);

/* ── Read-only access ───────────────────────────────────────────── */

const sigviz_state_t *sigviz_get_state(void);

#endif /* ZEOS_SIGVIZ_H */
