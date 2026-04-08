/*
 * Zeos — Web Browser
 *
 * Signal-native browser. The page load IS a signal chain:
 * URL → DNS → TCP → TLS → HTTP → parse → layout → render
 *
 * v1: HTML + basic CSS, no JavaScript, static web.
 * Renders to framebuffer via existing fb.c primitives.
 */

#include "browser.h"
#include "net_http.h"
#include "net_tls.h"
#include "fb.h"
#include "theme.h"
#include "kprint.h"

/* ── String helpers (bare-metal, no stdlib) ── */

static int str_len(const char *s) {
    int n = 0; while (s[n]) n++; return n;
}

static void str_copy(char *d, const char *s) {
    while (*s) *d++ = *s++; *d = 0;
}

static int str_eq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static int str_starts(const char *s, const char *prefix) {
    while (*prefix) {
        if (*s++ != *prefix++) return 0;
    }
    return 1;
}

static void mem_zero(void *p, int len) {
    uint8_t *b = (uint8_t *)p;
    for (int i = 0; i < len; i++) b[i] = 0;
}

/* ── DOM allocator (static pool, no malloc) ── */

#define DOM_POOL_SIZE 512
static dom_node_t dom_pool[DOM_POOL_SIZE];
static int dom_pool_next = 0;

static dom_node_t *dom_alloc(void) {
    if (dom_pool_next >= DOM_POOL_SIZE) return 0;
    dom_node_t *n = &dom_pool[dom_pool_next++];
    mem_zero(n, sizeof(*n));
    return n;
}

void dom_free(dom_node_t *root) {
    (void)root;
    dom_pool_next = 0;  /* Reset pool */
}

/* ── HTML Parser (minimal, handles real-world HTML) ── */

static void skip_ws(const char **p, const char *end) {
    while (*p < end && (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r'))
        (*p)++;
}

static int is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static char to_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

/* Read a tag name */
static int read_tag(const char **p, const char *end, char *out, int max) {
    int i = 0;
    while (*p < end && (is_alpha(**p) || **p == '-' || (**p >= '0' && **p <= '9'))) {
        if (i < max - 1) out[i++] = to_lower(**p);
        (*p)++;
    }
    out[i] = 0;
    return i;
}

/* Read attribute value (after =) */
static int read_attr_val(const char **p, const char *end, char *out, int max) {
    skip_ws(p, end);
    if (*p >= end) return 0;

    char quote = 0;
    if (**p == '"' || **p == '\'') { quote = **p; (*p)++; }

    int i = 0;
    while (*p < end) {
        if (quote && **p == quote) { (*p)++; break; }
        if (!quote && (**p == ' ' || **p == '>' || **p == '/')) break;
        if (i < max - 1) out[i++] = **p;
        (*p)++;
    }
    out[i] = 0;
    return i;
}

/* Self-closing tags */
static int is_void_tag(const char *tag) {
    return str_eq(tag, "br") || str_eq(tag, "hr") || str_eq(tag, "img") ||
           str_eq(tag, "input") || str_eq(tag, "meta") || str_eq(tag, "link") ||
           str_eq(tag, "area") || str_eq(tag, "base") || str_eq(tag, "col") ||
           str_eq(tag, "embed") || str_eq(tag, "source") || str_eq(tag, "wbr");
}

dom_node_t *html_parse(const char *html, int len) {
    dom_node_t *root = dom_alloc();
    if (!root) return 0;
    root->type = DOM_DOCUMENT;
    str_copy(root->tag, "document");

    dom_node_t *current = root;
    const char *p = html;
    const char *end = html + len;

    while (p < end) {
        if (*p == '<') {
            p++;
            skip_ws(&p, end);

            /* Closing tag */
            if (p < end && *p == '/') {
                p++;
                char tag[32];
                read_tag(&p, end, tag, 32);
                /* Skip to > */
                while (p < end && *p != '>') p++;
                if (p < end) p++;

                /* Walk up to matching parent */
                dom_node_t *walk = current;
                while (walk && walk != root) {
                    if (str_eq(walk->tag, tag)) {
                        current = walk->parent ? walk->parent : root;
                        break;
                    }
                    walk = walk->parent;
                }
                continue;
            }

            /* Comment */
            if (p + 2 < end && p[0] == '!' && p[1] == '-' && p[2] == '-') {
                while (p + 2 < end && !(p[0] == '-' && p[1] == '-' && p[2] == '>'))
                    p++;
                if (p + 2 < end) p += 3;
                continue;
            }

            /* DOCTYPE, script, style — skip */
            if (p < end && *p == '!') {
                while (p < end && *p != '>') p++;
                if (p < end) p++;
                continue;
            }

            /* Opening tag */
            char tag[32];
            read_tag(&p, end, tag, 32);

            /* Skip script/style content */
            if (str_eq(tag, "script") || str_eq(tag, "style")) {
                while (p < end && *p != '>') p++;
                if (p < end) p++;
                /* Skip until closing tag */
                while (p + 1 < end) {
                    if (*p == '<' && *(p+1) == '/') {
                        const char *check = p + 2;
                        char ctag[32];
                        read_tag(&check, end, ctag, 32);
                        if (str_eq(ctag, tag)) {
                            p = check;
                            while (p < end && *p != '>') p++;
                            if (p < end) p++;
                            break;
                        }
                    }
                    p++;
                }
                continue;
            }

            dom_node_t *node = dom_alloc();
            if (!node) break;
            node->type = DOM_ELEMENT;
            str_copy(node->tag, tag);
            node->parent = current;

            /* Add as child */
            if (!current->first_child) {
                current->first_child = node;
            } else {
                dom_node_t *sib = current->first_child;
                while (sib->next_sibling) sib = sib->next_sibling;
                sib->next_sibling = node;
            }

            /* Parse attributes */
            while (p < end && *p != '>' && *p != '/') {
                skip_ws(&p, end);
                if (p >= end || *p == '>' || *p == '/') break;

                char attr_name[64];
                int ai = 0;
                while (p < end && *p != '=' && *p != '>' && *p != '/' && *p != ' ') {
                    if (ai < 63) attr_name[ai++] = to_lower(*p);
                    p++;
                }
                attr_name[ai] = 0;

                skip_ws(&p, end);
                if (p < end && *p == '=') {
                    p++;
                    if (str_eq(attr_name, "href"))
                        read_attr_val(&p, end, node->attr_href, 512);
                    else if (str_eq(attr_name, "src"))
                        read_attr_val(&p, end, node->attr_src, 512);
                    else if (str_eq(attr_name, "class"))
                        read_attr_val(&p, end, node->attr_class, 256);
                    else if (str_eq(attr_name, "style"))
                        read_attr_val(&p, end, node->attr_style, 512);
                    else {
                        char dummy[512];
                        read_attr_val(&p, end, dummy, 512);
                    }
                }
            }

            /* Self-closing? */
            if (p < end && *p == '/') p++;
            if (p < end && *p == '>') p++;

            if (!is_void_tag(tag)) {
                current = node;
            }

        } else {
            /* Text node */
            const char *start = p;
            while (p < end && *p != '<') p++;

            /* Only create text node if non-whitespace */
            int has_content = 0;
            for (const char *c = start; c < p; c++) {
                if (*c != ' ' && *c != '\t' && *c != '\n' && *c != '\r') {
                    has_content = 1;
                    break;
                }
            }

            if (has_content) {
                dom_node_t *text = dom_alloc();
                if (!text) break;
                text->type = DOM_TEXT;
                text->parent = current;

                int tlen = (int)(p - start);
                if (tlen > 1023) tlen = 1023;

                /* Copy with whitespace normalization */
                int ti = 0;
                int last_space = 1;
                for (int i = 0; i < tlen && ti < 1023; i++) {
                    char c = start[i];
                    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
                    if (c == ' ' && last_space) continue;
                    text->text[ti++] = c;
                    last_space = (c == ' ');
                }
                text->text[ti] = 0;

                if (!current->first_child) {
                    current->first_child = text;
                } else {
                    dom_node_t *sib = current->first_child;
                    while (sib->next_sibling) sib = sib->next_sibling;
                    sib->next_sibling = text;
                }
            }
        }
    }

    return root;
}

/* ── Default Styles ── */

void css_apply_defaults(dom_node_t *node) {
    if (!node) return;

    /* Default text color and background from theme */
    node->style.color = COLOR_ON_SURFACE;
    node->style.background = 0;  /* Transparent */
    node->style.font_size = TYPE_BODY;
    node->style.font_weight = 400;
    node->style.display = 0;  /* block */

    /* Tag-specific defaults */
    if (str_eq(node->tag, "h1")) {
        node->style.font_size = TYPE_DISPLAY;
        node->style.font_weight = 700;
        node->style.margin[0] = 16; node->style.margin[2] = 8;
    } else if (str_eq(node->tag, "h2")) {
        node->style.font_size = TYPE_TITLE;
        node->style.font_weight = 700;
        node->style.margin[0] = 12; node->style.margin[2] = 6;
    } else if (str_eq(node->tag, "h3")) {
        node->style.font_size = TYPE_HEADING;
        node->style.font_weight = 700;
        node->style.margin[0] = 10; node->style.margin[2] = 4;
    } else if (str_eq(node->tag, "p")) {
        node->style.margin[0] = 8; node->style.margin[2] = 8;
    } else if (str_eq(node->tag, "a")) {
        node->style.color = COLOR_PRIMARY;  /* Persona accent for links */
        node->style.display = 1;  /* inline */
    } else if (str_eq(node->tag, "span") || str_eq(node->tag, "b") ||
               str_eq(node->tag, "i") || str_eq(node->tag, "em") ||
               str_eq(node->tag, "strong") || str_eq(node->tag, "code")) {
        node->style.display = 1;  /* inline */
    } else if (str_eq(node->tag, "strong") || str_eq(node->tag, "b")) {
        node->style.font_weight = 700;
    } else if (str_eq(node->tag, "code")) {
        node->style.font_size = TYPE_LABEL;  /* Slightly smaller for code */
    } else if (str_eq(node->tag, "hr")) {
        node->style.margin[0] = 8; node->style.margin[2] = 8;
    } else if (str_eq(node->tag, "li")) {
        node->style.margin[0] = 2; node->style.margin[2] = 2;
        node->style.padding[3] = 20;  /* Left indent */
    } else if (str_eq(node->tag, "ul") || str_eq(node->tag, "ol")) {
        node->style.margin[0] = 8; node->style.margin[2] = 8;
        node->style.padding[3] = 16;
    } else if (str_eq(node->tag, "head") || str_eq(node->tag, "meta") ||
               str_eq(node->tag, "title") || str_eq(node->tag, "link")) {
        node->style.display = 2;  /* none — hide head elements */
    }

    /* Recurse */
    for (dom_node_t *c = node->first_child; c; c = c->next_sibling)
        css_apply_defaults(c);
}

void css_apply_inline(dom_node_t *node) {
    /* TODO: parse style="" attributes for color, background, etc. */
    if (!node) return;
    for (dom_node_t *c = node->first_child; c; c = c->next_sibling)
        css_apply_inline(c);
}

/* ── Layout Engine ── */

void layout_compute(dom_node_t *root, int viewport_w, int viewport_h) {
    (void)viewport_h;
    if (!root) return;

    /* Simple block layout: stack blocks vertically, inline flows horizontally */
    int cursor_y = 0;

    for (dom_node_t *c = root->first_child; c; c = c->next_sibling) {
        if (c->style.display == 2) continue;  /* display:none */

        c->box.x = c->style.margin[3] + c->style.padding[3];
        c->box.y = cursor_y + c->style.margin[0];
        c->box.w = viewport_w - c->style.margin[1] - c->style.margin[3];

        if (c->type == DOM_TEXT) {
            /* Approximate text height: chars / chars_per_line * line_height */
            int tlen = str_len(c->text);
            int char_w = c->style.font_size * 6 / 10;  /* Approximate char width */
            if (char_w < 1) char_w = 8;
            int chars_per_line = c->box.w / char_w;
            if (chars_per_line < 1) chars_per_line = 1;
            int lines = (tlen + chars_per_line - 1) / chars_per_line;
            int line_h = c->style.font_size * 3 / 2;  /* 1.5x line height */
            c->box.h = lines * line_h;
        } else {
            /* Recurse into children to compute content height */
            layout_compute(c, c->box.w - c->style.padding[1] - c->style.padding[3],
                          viewport_h);

            /* Height = sum of children */
            int child_h = 0;
            for (dom_node_t *gc = c->first_child; gc; gc = gc->next_sibling) {
                int bottom = gc->box.y + gc->box.h + gc->style.margin[2];
                if (bottom > child_h) child_h = bottom;
            }
            c->box.h = child_h + c->style.padding[0] + c->style.padding[2];
        }

        cursor_y = c->box.y + c->box.h + c->style.margin[2];
    }
}

/* ── Renderer ── */

void render_page(dom_node_t *root, int ox, int oy, int scroll_y,
                 int viewport_w, int viewport_h)
{
    if (!root) return;

    for (dom_node_t *c = root->first_child; c; c = c->next_sibling) {
        if (c->style.display == 2) continue;

        int draw_x = ox + c->box.x;
        int draw_y = oy + c->box.y - scroll_y;

        /* Clip: skip if entirely outside viewport */
        if (draw_y + c->box.h < oy) continue;
        if (draw_y > oy + viewport_h) continue;

        /* Background */
        if (c->style.background) {
            fb_rect(draw_x, draw_y, c->box.w, c->box.h, c->style.background);
        }

        /* HR = horizontal line */
        if (str_eq(c->tag, "hr")) {
            fb_hline(draw_x, draw_y + c->box.h / 2, c->box.w, COLOR_SEPARATOR);
            continue;
        }

        /* Text */
        if (c->type == DOM_TEXT && c->text[0]) {
            /* Use fb_text with theme color */
            fb_text(draw_x, draw_y, c->text, c->style.color);
        }

        /* Recurse */
        render_page(c, draw_x + c->style.padding[3],
                    draw_y + c->style.padding[0],
                    0, /* scroll already applied at top level */
                    c->box.w, c->box.h);
    }
}

/* ── Browser State ── */

void browser_init(browser_t *b) {
    mem_zero(b, sizeof(*b));
    str_copy(b->url, "about:blank");
    b->history_pos = -1;
}

/* Parse URL into hostname + path */
static void parse_url(const char *url, char *hostname, int hmax,
                      char *path, int pmax, int *use_tls)
{
    *use_tls = 0;
    const char *p = url;

    if (str_starts(p, "https://")) { *use_tls = 1; p += 8; }
    else if (str_starts(p, "http://")) { p += 7; }

    /* Hostname */
    int hi = 0;
    while (*p && *p != '/' && *p != ':' && hi < hmax - 1)
        hostname[hi++] = *p++;
    hostname[hi] = 0;

    /* Skip port if present */
    if (*p == ':') { while (*p && *p != '/') p++; }

    /* Path */
    if (*p == '/') {
        int pi = 0;
        while (*p && pi < pmax - 1) path[pi++] = *p++;
        path[pi] = 0;
    } else {
        path[0] = '/'; path[1] = 0;
    }
}

int browser_navigate(browser_t *b, const char *url)
{
    str_copy(b->url, url);

    char hostname[256], path[1024];
    int use_tls = 0;
    parse_url(url, hostname, 256, path, 1024, &use_tls);
    str_copy(b->hostname, hostname);
    str_copy(b->path, path);

    kputs("BROWSE: ");
    kputs(url);
    kputs("\n");

    /* Free previous DOM */
    if (b->dom) {
        dom_free(b->dom);
        b->dom = 0;
    }

    /* Fetch page */
    static char page_buf[16384];
    int status = -1;

    if (use_tls) {
        int body_len = 0;
        status = https_get(hostname, path, page_buf, sizeof(page_buf), &body_len);
        b->page_len = body_len;
    } else {
        /* Use existing HTTP GET */
        struct http_response resp;
        extern int http_get(const char *hostname, const char *path,
                           struct http_response *resp);
        status = http_get(hostname, path, &resp);
        if (status >= 0) {
            int copy_len = resp.body_len;
            if (copy_len > (int)sizeof(page_buf) - 1) copy_len = sizeof(page_buf) - 1;
            for (int i = 0; i < copy_len; i++) page_buf[i] = resp.body[i];
            page_buf[copy_len] = 0;
            b->page_len = copy_len;
        }
    }

    b->http_status = status;

    if (status < 0) {
        kputs("BROWSE: fetch failed\n");
        return -1;
    }

    /* Parse HTML */
    b->dom = html_parse(page_buf, b->page_len);
    if (!b->dom) {
        kputs("BROWSE: parse failed\n");
        return -1;
    }

    /* Apply styles */
    css_apply_defaults(b->dom);
    css_apply_inline(b->dom);

    /* Compute layout */
    int content_w = b->surface_w - 16;  /* 8px padding each side */
    layout_compute(b->dom, content_w, b->surface_h - 96);  /* toolbar + status = ~96px */

    /* Compute total page height */
    b->page_height = 0;
    for (dom_node_t *c = b->dom->first_child; c; c = c->next_sibling) {
        int bottom = c->box.y + c->box.h + c->style.margin[2];
        if (bottom > b->page_height) b->page_height = bottom;
    }

    b->scroll_y = 0;

    /* Add to history */
    if (b->history_pos < BROWSER_HISTORY_MAX - 1) {
        b->history_pos++;
        str_copy(b->history[b->history_pos], url);
        b->history_len = b->history_pos + 1;
    }

    /* Count DOM nodes */
    b->node_count = dom_pool_next;

    kputs("BROWSE: ");
    kput_dec(status);
    kputs(" OK, ");
    kput_dec(b->node_count);
    kputs(" nodes, ");
    kput_dec(b->page_height);
    kputs("px\n");

    return 0;
}

void browser_back(browser_t *b) {
    if (b->history_pos > 0) {
        b->history_pos--;
        browser_navigate(b, b->history[b->history_pos]);
    }
}

void browser_forward(browser_t *b) {
    if (b->history_pos < b->history_len - 1) {
        b->history_pos++;
        browser_navigate(b, b->history[b->history_pos]);
    }
}

void browser_refresh(browser_t *b) {
    browser_navigate(b, b->url);
}

void browser_home(browser_t *b) {
    browser_navigate(b, "http://example.com");
}

void browser_scroll(browser_t *b, int delta_y) {
    b->scroll_y += delta_y;
    if (b->scroll_y < 0) b->scroll_y = 0;
    int max_scroll = b->page_height - (b->surface_h - 96);
    if (max_scroll < 0) max_scroll = 0;
    if (b->scroll_y > max_scroll) b->scroll_y = max_scroll;
}

void browser_click(browser_t *b, int x, int y) {
    /* TODO: hit-test DOM nodes, follow links */
    (void)b; (void)x; (void)y;
}

void browser_draw(browser_t *b) {
    if (!b->dom) return;

    /* Clear content area */
    int cx = b->surface_x + 8;
    int cy = b->surface_y + 48;  /* Below toolbar */
    int cw = b->surface_w - 16;
    int ch = b->surface_h - 96;  /* Toolbar + status */

    fb_rect(cx, cy, cw, ch, COLOR_SURFACE);

    /* Render page */
    render_page(b->dom, cx, cy, b->scroll_y, cw, ch);
}

void browser_draw_toolbar(browser_t *b) {
    int tx = b->surface_x;
    int ty = b->surface_y;
    int tw = b->surface_w;

    /* Toolbar background */
    fb_rect(tx, ty, tw, 48, COLOR_SURFACE_HIGH);

    /* Navigation buttons (text placeholders until icon rendering works) */
    fb_text(tx + 8, ty + 16, "<", COLOR_ON_SURFACE);       /* Back */
    fb_text(tx + 24, ty + 16, ">", COLOR_ON_SURFACE);      /* Forward */
    fb_text(tx + 40, ty + 16, "R", COLOR_ON_SURFACE);      /* Refresh */

    /* URL bar */
    fb_rect(tx + 64, ty + 8, tw - 128, 32, COLOR_SURFACE_TOP);
    fb_text(tx + 72, ty + 16, b->url, COLOR_ON_SURFACE_2);

    /* Separator */
    fb_hline(tx, ty + 47, tw, COLOR_SEPARATOR);
}

void browser_draw_status(browser_t *b) {
    int sx = b->surface_x;
    int sy = b->surface_y + b->surface_h - 24;
    int sw = b->surface_w;

    fb_rect(sx, sy, sw, 24, COLOR_SURFACE_HIGH);
    fb_hline(sx, sy, sw, COLOR_SEPARATOR);

    /* Status text */
    char status[128];
    status[0] = 0;

    /* Build status string manually (no sprintf) */
    char *p = status;
    *p++ = 'H'; *p++ = 'T'; *p++ = 'T'; *p++ = 'P'; *p++ = ' ';

    /* Status code */
    if (b->http_status > 0) {
        *p++ = '0' + (b->http_status / 100);
        *p++ = '0' + ((b->http_status / 10) % 10);
        *p++ = '0' + (b->http_status % 10);
    } else {
        *p++ = '-';
    }

    *p++ = ' '; *p++ = '|'; *p++ = ' ';
    *p++ = 'n'; *p++ = 'o'; *p++ = 'd'; *p++ = 'e'; *p++ = 's';
    *p++ = ':'; *p++ = ' ';

    /* Node count (simple int to string) */
    int nc = b->node_count;
    if (nc >= 100) *p++ = '0' + (nc / 100);
    if (nc >= 10) *p++ = '0' + ((nc / 10) % 10);
    *p++ = '0' + (nc % 10);

    *p = 0;

    fb_text(sx + 8, sy + 4, status, COLOR_ON_SURFACE_3);
}
