/*
 * Zeos — Signal Chain Visualizer
 *
 * Draws signal chains as box-and-arrow diagrams on the framebuffer.
 * Uses the fb drawing primitives (fb_rect, fb_line, fb_text, etc).
 *
 * Layout: vertical stack with arrows between nodes.
 * Each node is a labeled box. Color indicates state.
 * Edges drawn as arrows between boxes.
 */

#include "sigviz.h"
#include "signal.h"
#include "fb.h"

/* Node box dimensions */
#define NODE_W      120
#define NODE_H      28
#define NODE_PAD    16      /* Vertical spacing between nodes */
#define ARROW_LEN   12      /* Arrow head length */
#define MARGIN      8       /* Margin from draw area edges */

/* String length helper */
static int sviz_strlen(const char *s)
{
    int len = 0;
    while (s[len]) len++;
    return len;
}

/* Get color for a node state */
static uint32_t state_color(enum sig_node_state state)
{
    switch (state) {
    case SIG_IDLE:    return SIGVIZ_IDLE;
    case SIG_READY:   return SIGVIZ_READY;
    case SIG_RUNNING: return SIGVIZ_RUNNING;
    case SIG_DONE:    return SIGVIZ_DONE;
    case SIG_ERROR:   return SIGVIZ_ERROR;
    }
    return SIGVIZ_IDLE;
}

/* Draw an arrow pointing down from (x, y1) to (x, y2) */
static void draw_arrow_down(int x, int y1, int y2, uint32_t color)
{
    /* Vertical line */
    fb_vline(x, y1, y2 - y1, color);

    /* Arrow head */
    int tip = y2;
    fb_line(x, tip, x - 4, tip - 8, color);
    fb_line(x, tip, x + 4, tip - 8, color);
}

void sigviz_draw(int chain_id, int x, int y, int w, int h)
{
    struct sig_chain *chain = sig_get_chain(chain_id);
    if (!chain || chain->node_count == 0)
        return;

    /* Background */
    fb_rect(x, y, w, h, SIGVIZ_BG);

    /* Title bar */
    fb_rect(x, y, w, 20, 0x001A1A2E);
    fb_text(x + 4, y + 2, chain->name, SIGVIZ_TEXT);

    /* Calculate layout */
    int node_count = chain->node_count;
    int total_height = node_count * (NODE_H + NODE_PAD) - NODE_PAD;
    int start_y = y + 24 + (h - 24 - total_height) / 2;
    if (start_y < y + 24) start_y = y + 24;

    int center_x = x + w / 2;

    /* Draw each node */
    for (int i = 0; i < node_count; i++) {
        struct sig_node *node = &chain->nodes[i];

        int nx = center_x - NODE_W / 2;
        int ny = start_y + i * (NODE_H + NODE_PAD);

        /* Node background */
        fb_rect(nx, ny, NODE_W, NODE_H, SIGVIZ_NODE_BG);

        /* State indicator (left bar) */
        uint32_t sc = state_color(node->state);
        fb_rect(nx, ny, 4, NODE_H, sc);

        /* Node name */
        int name_len = sviz_strlen(node->name);
        int text_x = nx + 8;
        int text_y = ny + 6;
        fb_text(text_x, text_y, node->name, SIGVIZ_TEXT);

        /* Timing (if done) */
        if (node->state == SIG_DONE && node->tsc_end > node->tsc_start) {
            uint64_t cycles = node->tsc_end - node->tsc_start;

            /* Timing bar — proportional width */
            uint64_t max_cycles = 5000;  /* Scale reference */
            int bar_max = NODE_W - name_len * 8 - 16;
            if (bar_max < 20) bar_max = 20;
            int bar_w = (int)(cycles * (uint64_t)bar_max / max_cycles);
            if (bar_w > bar_max) bar_w = bar_max;
            if (bar_w < 2) bar_w = 2;

            int bar_x = nx + NODE_W - bar_w - 4;
            int bar_y = ny + NODE_H - 6;
            fb_rect(bar_x, bar_y, bar_w, 3, sc);
        }

        /* Border */
        fb_rect_outline(nx, ny, NODE_W, NODE_H, SIGVIZ_BORDER, 1);

        /* Draw arrows to downstream nodes */
        for (int e = 0; e < node->output_count; e++) {
            int dst_idx = node->output_nodes[e];
            if (dst_idx >= 0 && dst_idx < node_count) {
                int arrow_start_y = ny + NODE_H;
                int arrow_end_y = start_y + dst_idx * (NODE_H + NODE_PAD);

                /* Only draw if destination is below */
                if (arrow_end_y > arrow_start_y) {
                    /* Offset arrows for multiple edges */
                    int arrow_x = center_x + (e - node->output_count / 2) * 12;
                    draw_arrow_down(arrow_x, arrow_start_y + 2, arrow_end_y - 2, SIGVIZ_EDGE);
                }
            }
        }
    }

    /* Chain timing at bottom */
    if (chain->resolve_count > 0) {
        int info_y = y + h - 18;
        fb_text(x + 4, info_y, "Resolved: ", 0x005F574F);

        /* Convert cycles to simple number text */
        char buf[32];
        uint64_t total = chain->tsc_end - chain->tsc_start;
        int bi = 0;
        if (total == 0) {
            buf[bi++] = '0';
        } else {
            char tmp[21];
            int ti = 20;
            tmp[ti] = '\0';
            while (total > 0) {
                tmp[--ti] = '0' + (total % 10);
                total /= 10;
            }
            while (tmp[ti])
                buf[bi++] = tmp[ti++];
        }
        buf[bi] = '\0';

        fb_text(x + 84, info_y, buf, 0x005F574F);
        fb_text(x + 84 + bi * 8 + 4, info_y, "cy", 0x005F574F);
    }
}
