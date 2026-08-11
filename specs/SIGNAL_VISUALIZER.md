# Signal Chain Visualizer — Specification

> Students need to SEE data flowing. Not read hex dumps.
>
> **Date**: March 22, 2026
> **Status**: Specification — for kernel agent
> **Depends on**: signal.h, fb.h (framebuffer)

---

## What It Does

Draws the signal chain graph on screen. Nodes are boxes. Edges are lines.
Data flows visually — you can watch a value travel from source to sink.

This is not a debugging tool (though it is). It's a **teaching tool**.
A student types `signal` and sees a pipeline animate on screen.

---

## Display Modes

### 1. Text Trace (already implemented in shell.c `trace` command)
```
  [Source] 142cy ##
    |
    v
  [Double] 89cy #
    |
    v
  [Display] 201cy ##
```

### 2. Graphical (needs framebuffer drawing — kernel agent work)

Each node is a rectangle drawn on the framebuffer:
- Width: proportional to name length + padding
- Height: fixed (24px)
- Color: state-dependent
  - Idle:    dark gray (#333333)
  - Ready:   yellow (#CCAA00)
  - Running: bright white (#FFFFFF)
  - Done:    green (#00AA44)
  - Error:   red (#AA0000)
- Edges: vertical or horizontal lines connecting boxes
- Data flow: a bright pixel/dot that moves along the edge during resolution

### 3. Live Mode (future — needs timer interrupt + redraw)

The visualizer runs continuously, redrawing as chains resolve.
Useful for robotics: student sees sensor → decision → motor in real time.

---

## Kernel Requirements

### New functions needed in fb.h/fb.c:
```c
/* Draw a filled rectangle */
void fb_rect(int x, int y, int w, int h, uint32_t color);

/* Draw a horizontal/vertical line */
void fb_hline(int x, int y, int w, uint32_t color);
void fb_vline(int x, int y, int h, uint32_t color);

/* Draw text at pixel position (not character grid) */
void fb_text(int x, int y, const char *s, uint32_t color);
```

### New file: os/boot/sigviz.c
```c
/* Render a signal chain as a graphical diagram */
void sigviz_draw(int chain_id, int x, int y);

/* Animate one resolution step (call between sig_resolve iterations) */
void sigviz_animate(int chain_id);

/* Set visualization area bounds */
void sigviz_bounds(int x, int y, int w, int h);
```

### Hook into signal.c

The resolver (`sig_resolve`) should call a viz callback between
node firings so the visualizer can animate:

```c
/* Optional viz callback — called after each node fires */
typedef void (*sig_viz_fn)(int chain_id, int node_idx, enum sig_node_state state);
void sig_set_viz(sig_viz_fn callback);
```

---

## Layout Algorithm

For the initial version, simple vertical stack:
1. Source nodes at top
2. Each layer = one edge depth
3. Nodes in same layer arranged horizontally
4. Edges drawn as vertical lines with corners for horizontal spans

This handles the common case (linear pipelines) and most tree shapes.
Complex DAGs can use the text trace mode until a proper layout engine exists.

---

## Priority

1. `fb_rect`, `fb_hline`, `fb_vline` — framebuffer drawing primitives
2. `sigviz_draw` — static diagram of a chain
3. `sig_set_viz` callback — hook into resolver
4. `sigviz_animate` — animated data flow
5. Live mode with timer-driven redraw
