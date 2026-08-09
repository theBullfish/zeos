/*
 * Zeos — Web Browser (Chain Surface)
 *
 * A signal-native web browser rendered as a chain surface.
 * The browser IS a signal chain:
 *
 *   [URL input] → [DNS] → [TCP/TLS] → [HTTP] → [HTML parse] → [layout] → [render]
 *
 * Each node in the chain is inspectable via the signal inspector.
 * You can tap any node to see its current state (DNS cache hit?
 * TLS cipher? HTTP status? DOM tree? Layout boxes?).
 *
 * Rendering engine: minimal HTML/CSS renderer.
 * - Block/inline layout
 * - Text rendering via Inter font (TTF rasterizer)
 * - Basic CSS: color, background, margin, padding, border, font-size,
 *   font-weight, display, width, height, text-align
 * - Links (clickable, persona-accent colored)
 * - Images (PNG decode, render into layout)
 * - No JavaScript (v1 — static web only)
 * - No flexbox/grid (v1 — block/inline is sufficient)
 *
 * Chain surface layout:
 * ┌─[×][−][□][⚡]──────────────────────────────────────────────┐
 * │ [←] [→] [⟳] [⌂]  [  URL bar                    ] [☆] [⋮] │
 * ├─────────────────────────────────────────────────────────────┤
 * │                                                             │
 * │                    Rendered web page                        │
 * │                                                             │
 * │                                                             │
 * ├─────────────────────────────────────────────────────────────┤
 * │ ⚡ DNS: 1ms │ TLS: P-256 │ HTTP: 200 │ nodes: 47 │ 0.3s   │
 * └─────────────────────────────────────────────────────────────┘
 *
 * Icons (from assets/icons/):
 *   ← → : arrows/Arrow Left.svg, arrows/Arrow Right.svg
 *   ⟳   : arrows/Refresh Right Redo.svg
 *   ⌂   : ui-basic/Home.svg (or similar)
 *   ☆   : ui-basic/Bookmark 1.svg
 *   ⋮   : ui-basic/More Vertical.svg
 */

#ifndef ZEOS_BROWSER_H
#define ZEOS_BROWSER_H

#include <stdint.h>

/* ── DOM Node Types ── */
typedef enum {
    DOM_DOCUMENT,
    DOM_ELEMENT,
    DOM_TEXT,
} dom_node_type_t;

/* ── DOM Node ── */
typedef struct dom_node {
    dom_node_type_t type;
    char            tag[32];       /* "div", "p", "a", "h1", etc. */
    char            text[1024];    /* Text content (for DOM_TEXT) */

    /* Attributes (simplified — key/value pairs) */
    char            attr_href[512];
    char            attr_src[512];
    char            attr_class[256];
    char            attr_style[512];
    char            attr_type[32];     /* input type="text|password|..." */
    char            attr_value[256];   /* input/button value */
    char            attr_alt[256];     /* img alt text */
    char            attr_placeholder[256]; /* input placeholder */
    char            attr_id[128];      /* element id (getElementById) */
    char           *script_src;        /* <script> body (kmalloc'd), else NULL */

    /* CSS computed style (simplified) */
    struct {
        uint32_t color;            /* Text color (0xAARRGGBB) */
        uint32_t background;       /* Background color */
        int      font_size;        /* Pixels */
        int      font_weight;      /* 400=normal, 700=bold */
        int      margin[4];        /* top, right, bottom, left */
        int      padding[4];
        int      display;          /* 0=block, 1=inline, 2=none */
        int      text_align;       /* 0=left, 1=center, 2=right */
    } style;

    /* Layout box (computed during layout pass) */
    struct {
        int x, y;                  /* Position on page */
        int w, h;                  /* Dimensions */
    } box;

    /* Tree structure */
    struct dom_node *parent;
    struct dom_node *first_child;
    struct dom_node *next_sibling;

    /* Decoded image cache (for <img>). NULL if not fetched/decoded.
     * img_pixels is a kmalloc'd RGBA buffer (4 * img_w * img_h bytes). */
    uint8_t        *img_pixels;
    int             img_w;
    int             img_h;
} dom_node_t;

/* ── HTML Parser ── */

/* Parse HTML string into DOM tree. Returns root node. */
dom_node_t *html_parse(const char *html, int len);

/* Free a DOM tree */
void dom_free(dom_node_t *root);

/* ── CSS Parser (inline + basic selectors) ── */

/* Apply default browser styles to DOM tree */
void css_apply_defaults(dom_node_t *root);

/* Parse and apply inline style="" attributes */
void css_apply_inline(dom_node_t *root);

/* ── Layout Engine ── */

/*
 * Compute layout boxes for all nodes.
 * viewport_w/h = available rendering area.
 */
void layout_compute(dom_node_t *root, int viewport_w, int viewport_h);

/* ── Renderer ── */

/*
 * Render the laid-out DOM tree to the framebuffer.
 * ox, oy = origin (top-left of content area in chain surface)
 * scroll_y = vertical scroll offset
 */
void render_page(dom_node_t *root, int ox, int oy, int scroll_y,
                 int viewport_w, int viewport_h);

/* ── Browser State ── */

typedef struct {
    /* Navigation */
    char    url[2048];
    char    hostname[256];
    char    path[1024];

    /* Page state */
    dom_node_t *dom;               /* Current page DOM */
    char       *page_source;       /* Raw HTML */
    int         page_len;

    /* Scroll */
    int scroll_y;
    int page_height;               /* Total computed height */

    /* History (simple ring buffer) */
    #define BROWSER_HISTORY_MAX 32
    char    history[BROWSER_HISTORY_MAX][2048];
    int     history_pos;
    int     history_len;

    /* Status (displayed in status bar) */
    int     http_status;
    int     dns_ms;
    int     tls_cipher;            /* 0 = none/http */
    int     node_count;
    int     load_time_ms;

    /* Chain surface integration */
    int     surface_x, surface_y;
    int     surface_w, surface_h;
} browser_t;

/* Initialize browser */
void browser_init(browser_t *b);

/* Browser-as-app: open (or focus) the browser surface + navigate. */
int  browser_app_open(const char *url);
int  browser_app_active(void);
void browser_app_click(int x, int y);

/* Navigate to URL */
int browser_navigate(browser_t *b, const char *url);

/* Navigation controls */
void browser_back(browser_t *b);
void browser_forward(browser_t *b);
void browser_refresh(browser_t *b);
void browser_home(browser_t *b);

/* Scroll */
void browser_scroll(browser_t *b, int delta_y);

/* Keyboard input (scancodes: arrows, pgup/pgdn, home/end) */
void browser_key(browser_t *b, uint8_t scancode);

/* Mouse wheel scroll (delta: +1 down, -1 up per notch) */
void browser_mouse_wheel(browser_t *b, int delta);

/* Handle click at viewport coordinates */
void browser_click(browser_t *b, int x, int y);

/* Right-click on a link (returns 1 if a context menu opened). */
int  browser_right_click(browser_t *b, int x, int y);

/* URL bar editing — records undo, marks surface dirty. */
void browser_url_type(browser_t *b, char ch);
void browser_url_backspace(browser_t *b);

/* Render history list state (LOADING/EMPTY/ERROR) into a rect. */
void browser_history_render_state(browser_t *b, int x, int y, int w, int h);

/* Scrollbar interaction — returns 1 if click was on scrollbar */
int browser_scrollbar_click(browser_t *b, int x, int y);

/* Drag scrollbar thumb (call each frame while mouse held) */
void browser_scrollbar_drag(browser_t *b, int y);

/* Release scrollbar drag */
void browser_scrollbar_release(void);

/* Render current page into chain surface area */
void browser_draw(browser_t *b);

/* Draw the toolbar (back, forward, refresh, URL bar, etc.) */
void browser_draw_toolbar(browser_t *b);

/* Draw the status bar (DNS, TLS, HTTP status, timing) */
void browser_draw_status(browser_t *b);

#endif /* ZEOS_BROWSER_H */
