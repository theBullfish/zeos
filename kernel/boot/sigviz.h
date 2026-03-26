/*
 * Zeos — Signal Chain Visualizer
 *
 * Draws signal chains as graphical diagrams on the framebuffer.
 * Students see data flow. Engineers see timing.
 * Same data, different views. MasQ at the display level.
 */

#ifndef ZEOS_SIGVIZ_H
#define ZEOS_SIGVIZ_H

#include <stdint.h>

/* Draw a signal chain as a graphical diagram.
 * chain_id: which chain to draw
 * x, y: top-left corner on screen
 * w, h: available drawing area
 */
void sigviz_draw(int chain_id, int x, int y, int w, int h);

/* Colors for node states — derived from theme.h design tokens */
#include "theme.h"

#define SIGVIZ_IDLE      COLOR_ON_SURFACE_4   /* quaternary text — idle */
#define SIGVIZ_READY     COLOR_WARNING        /* amber — ready to fire */
#define SIGVIZ_RUNNING   COLOR_PRIMARY        /* persona accent — active */
#define SIGVIZ_DONE      COLOR_SUCCESS        /* teal — complete */
#define SIGVIZ_ERROR     COLOR_DANGER         /* red — error */
#define SIGVIZ_EDGE      COLOR_ON_SURFACE_3   /* tertiary text — edges */
#define SIGVIZ_TEXT       COLOR_ON_SURFACE     /* primary text */
#define SIGVIZ_BG        COLOR_SURFACE        /* background */
#define SIGVIZ_NODE_BG   COLOR_SURFACE_HIGH   /* raised surface — node fill */
#define SIGVIZ_BORDER    COLOR_SEPARATOR      /* border/divider */

/* Get current persona's accent for node highlights (defined in shell.c) */
uint32_t theme_accent(void);
uint32_t theme_accent_dim(void);

#endif /* ZEOS_SIGVIZ_H */
