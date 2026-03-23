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

/* Colors for node states */
#define SIGVIZ_IDLE      0x00333333
#define SIGVIZ_READY     0x00CCAA00
#define SIGVIZ_RUNNING   0x00FFFFFF
#define SIGVIZ_DONE      0x0000AA44
#define SIGVIZ_ERROR     0x00AA0000
#define SIGVIZ_EDGE      0x0029ADFF
#define SIGVIZ_TEXT       0x00FFF1E8
#define SIGVIZ_BG        0x000D1117
#define SIGVIZ_NODE_BG   0x001A1A2E
#define SIGVIZ_BORDER    0x00333333

#endif /* ZEOS_SIGVIZ_H */
