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
#include "font.h"
#include "theme.h"
#include "mouse.h"
#include "kprint.h"
#include "heap.h"
#include "lodepng/lodepng.h"
#include "ui_undo.h"
#include "ui_hover.h"
#include "ui_dirty.h"
#include "ui_context_menu.h"
#include "ui_states.h"

/* ── UI primitive wiring (call-sites) ── */
#define BROWSER_WIN_ID 4001
static undo_buffer_t s_url_undo;
static int s_url_undo_inited = 0;
static uint64_t s_hov_back, s_hov_fwd, s_hov_refresh, s_hov_home, s_hov_url;
static int s_hov_inited = 0;
#define BROWSER_MAX_LINK_HOVERS 256
static uint64_t s_link_tokens[BROWSER_MAX_LINK_HOVERS];
static int s_link_token_count = 0;
static browser_t *s_rc_browser = 0;
static char s_rc_href[2048];
static void browser_url_apply(void *ctx, int pos,
                              const char *old_text, int old_len,
                              const char *new_text, int new_len) {
    browser_t *b = (browser_t *)ctx; if (!b) return;
    int url_len = 0; while (b->url[url_len]) url_len++;
    if (pos < 0) pos = 0; if (pos > url_len) pos = url_len;
    int tail_len = url_len - (pos + old_len); if (tail_len < 0) tail_len = 0;
    char tail[2048];
    for (int i = 0; i < tail_len && i < 2047; i++) tail[i] = b->url[pos + old_len + i];
    int wp = pos;
    for (int i = 0; i < new_len && wp < 2047; i++) b->url[wp++] = new_text[i];
    for (int i = 0; i < tail_len && wp < 2047; i++) b->url[wp++] = tail[i];
    b->url[wp] = 0; (void)old_text;
}
static void browser_ensure_undo(browser_t *b) {
    if (s_url_undo_inited) return;
    undo_init(&s_url_undo, browser_url_apply, b);
    s_url_undo_inited = 1;
    undo_set_focus(&s_url_undo);
}

/* ── lodepng allocator shims ──
 * lodepng was compiled with -DLODEPNG_NO_COMPILE_ALLOCATORS, so it expects
 * us to provide these. Route to the kernel heap. lodepng_realloc must be
 * a real realloc: copy old contents and free the old block.
 *
 * The kernel heap exposes kmalloc(size) / kfree(ptr) but not a sized header
 * for realloc, so we prefix every allocation with a size_t length. */

typedef struct { uint64_t size; } lp_hdr_t;

void *lodepng_malloc(size_t size);
void *lodepng_realloc(void *ptr, size_t new_size);
void  lodepng_free(void *ptr);

void *lodepng_malloc(size_t size) {
    uint8_t *raw = (uint8_t *)kmalloc(size + sizeof(lp_hdr_t));
    if (!raw) return 0;
    ((lp_hdr_t *)raw)->size = size;
    return raw + sizeof(lp_hdr_t);
}

void lodepng_free(void *ptr) {
    if (!ptr) return;
    kfree((uint8_t *)ptr - sizeof(lp_hdr_t));
}

void *lodepng_realloc(void *ptr, size_t new_size) {
    if (!ptr) return lodepng_malloc(new_size);
    uint64_t old_size = ((lp_hdr_t *)((uint8_t *)ptr - sizeof(lp_hdr_t)))->size;
    void *np = lodepng_malloc(new_size);
    if (!np) return 0;
    uint64_t copy = old_size < new_size ? old_size : new_size;
    uint8_t *d = (uint8_t *)np;
    uint8_t *s = (uint8_t *)ptr;
    for (uint64_t i = 0; i < copy; i++) d[i] = s[i];
    lodepng_free(ptr);
    return np;
}

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

/* Each dom_node_t is ~3.7 KB (text+attr buffers inline). 2048 nodes = ~7.5 MB.
 * Real fix would be separate text/attr pools, but bumping the count is the
 * lowest-effort way to stop truncating real-world pages. */
#define DOM_POOL_SIZE 2048
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
    /* Free any decoded image buffers before resetting the pool */
    for (int i = 0; i < dom_pool_next; i++) {
        if (dom_pool[i].img_pixels) {
            lodepng_free(dom_pool[i].img_pixels);
            dom_pool[i].img_pixels = 0;
            dom_pool[i].img_w = 0;
            dom_pool[i].img_h = 0;
        }
        if (dom_pool[i].script_src) {
            extern void kfree(void *);
            kfree(dom_pool[i].script_src);
            dom_pool[i].script_src = 0;
        }
    }
    dom_pool_next = 0;  /* Reset pool */
}

/* ── DOM manipulation API for the JS bindings (qjs_dom.c) ──────────────
 * These own all node mutation (the pool is private to browser.c). A mutation
 * sets g_dom_dirty so browser_navigate re-lays-out + repaints after scripts. */
static int g_dom_dirty = 0;
int dom_take_dirty(void) { int d = g_dom_dirty; g_dom_dirty = 0; return d; }

dom_node_t *dom_get_by_id(dom_node_t *node, const char *id) {
    if (!node || !id) return 0;
    for (dom_node_t *c = node->first_child; c; c = c->next_sibling) {
        if (c->type == DOM_ELEMENT && c->attr_id[0] && str_eq(c->attr_id, id))
            return c;
        dom_node_t *r = dom_get_by_id(c, id);
        if (r) return r;
    }
    return 0;
}

const char *dom_node_tag(dom_node_t *el) { return el ? el->tag : ""; }
const char *dom_node_id(dom_node_t *el)  { return el ? el->attr_id : ""; }
dom_node_t *dom_first_child(dom_node_t *el)  { return el ? el->first_child : 0; }
dom_node_t *dom_next_sibling(dom_node_t *el) { return el ? el->next_sibling : 0; }
const char *dom_script_src(dom_node_t *el)   { return (el && el->script_src) ? el->script_src : 0; }

/* document.createElement(tag) — new detached element from the pool. */
dom_node_t *dom_create_element(const char *tag) {
    dom_node_t *n = dom_alloc();
    if (!n) return 0;
    n->type = DOM_ELEMENT;
    int i = 0; if (tag) for (; tag[i] && i < 31; i++) n->tag[i] = tag[i];
    n->tag[i] = 0;
    return n;
}

/* document.createTextNode(s) */
dom_node_t *dom_create_text(const char *s) {
    dom_node_t *n = dom_alloc();
    if (!n) return 0;
    n->type = DOM_TEXT;
    int i = 0; if (s) for (; s[i] && i < 1023; i++) n->text[i] = s[i];
    n->text[i] = 0;
    return n;
}

/* parent.appendChild(child) — link child as the last child. */
void dom_append_child(dom_node_t *parent, dom_node_t *child) {
    if (!parent || !child) return;
    child->parent = parent;
    child->next_sibling = 0;
    if (!parent->first_child) parent->first_child = child;
    else {
        dom_node_t *s = parent->first_child;
        while (s->next_sibling) s = s->next_sibling;
        s->next_sibling = child;
    }
    g_dom_dirty = 1;
}

static void parse_url(const char *url, char *hostname, int hmax,
                      char *path, int pmax, int *use_tls);

/* JS fetch(): blocking HTTP(S) GET of a URL into body_out (NUL-terminated up to
 * max-1). Fills *status_out. Returns 0 on success, -1 on failure. Reuses the
 * same parse_url + http_get/https_get the browser navigation uses. */
int zeos_http_fetch(const char *url, char *body_out, int max, int *status_out) {
    if (!url || !body_out || max <= 1) return -1;
    char hostname[256], path[1024];
    int use_tls = 0;
    parse_url(url, hostname, 256, path, 1024, &use_tls);
    int status = -1, len = 0;
    if (use_tls) {
        status = https_get(hostname, path, body_out, max, &len);
        if (len < 0) len = 0;
        if (len > max - 1) len = max - 1;
        body_out[len] = 0;
    } else {
        /* static, not stack: http_response.body is ~256 KB and this runs deep
         * in the JS call chain (…JS_Eval->js_fetch) where a stack struct that
         * big overflows. */
        static struct http_response resp;
        extern int http_get(const char *host, const char *path, struct http_response *resp);
        if (http_get(hostname, path, &resp) < 0) { body_out[0] = 0; return -1; }
        status = resp.status_code;
        len = (int)resp.body_len;
        if (len > max - 1) len = max - 1;
        for (int i = 0; i < len; i++) body_out[i] = resp.body[i];
        body_out[len] = 0;
    }
    if (status_out) *status_out = status;
    return status >= 100 ? 0 : -1;
}

/* Is `cls` one of the space-separated tokens in `classlist`? */
static int dom_class_has(const char *classlist, const char *cls) {
    if (!classlist || !cls || !cls[0]) return 0;
    int cl = 0; while (cls[cl]) cl++;
    const char *p = classlist;
    while (*p) {
        while (*p == ' ') p++;
        const char *start = p;
        while (*p && *p != ' ') p++;
        int len = (int)(p - start);
        if (len == cl) {
            int m = 1;
            for (int i = 0; i < cl; i++) if (start[i] != cls[i]) { m = 0; break; }
            if (m) return 1;
        }
    }
    return 0;
}

static int dom_matches(dom_node_t *c, char type, const char *val) {
    if (c->type != DOM_ELEMENT) return 0;
    if (type == '#') return c->attr_id[0] && str_eq(c->attr_id, val);
    if (type == '.') return dom_class_has(c->attr_class, val);
    return str_eq(c->tag, val);
}

static void dom_selparse(const char *sel, char *type, const char **val) {
    if (sel[0] == '#') { *type = '#'; *val = sel + 1; }
    else if (sel[0] == '.') { *type = '.'; *val = sel + 1; }
    else { *type = 'T'; *val = sel; }
}

/* querySelector: first element matching a single #id / .class / tag selector. */
static dom_node_t *dom_query_rec(dom_node_t *node, char type, const char *val) {
    for (dom_node_t *c = node->first_child; c; c = c->next_sibling) {
        if (dom_matches(c, type, val)) return c;
        dom_node_t *r = dom_query_rec(c, type, val);
        if (r) return r;
    }
    return 0;
}
dom_node_t *dom_query(dom_node_t *root, const char *sel) {
    if (!sel || !root) return 0;
    char type; const char *val;
    dom_selparse(sel, &type, &val);
    return dom_query_rec(root, type, val);
}

/* querySelectorAll: walk in document order, calling visit() on each match.
 * (Callback keeps the pool private to browser.c; qjs_dom builds the JS array.) */
void dom_query_all(dom_node_t *node, const char *sel,
                   void (*visit)(dom_node_t *, void *), void *ctx) {
    char type; const char *val;
    if (!sel || !node) return;
    dom_selparse(sel, &type, &val);
    /* recurse manually so we can pass the parsed selector down */
    for (dom_node_t *c = node->first_child; c; c = c->next_sibling) {
        if (dom_matches(c, type, val)) visit(c, ctx);
        dom_query_all(c, sel, visit, ctx);
    }
}

/* innerHTML = html : parse the fragment and replace the element's children. */
void dom_set_inner_html(dom_node_t *el, const char *html) {
    if (!el || !html) return;
    int len = 0; while (html[len]) len++;
    dom_node_t *frag = html_parse(html, len);   /* document node with children */
    if (!frag) return;
    el->first_child = frag->first_child;         /* replace children */
    for (dom_node_t *c = el->first_child; c; c = c->next_sibling)
        c->parent = el;
    g_dom_dirty = 1;
}

/* Append raw text (bounded) to buf at *pos. */
static void dom_emit(char *buf, int *pos, int max, const char *s) {
    while (*s && *pos < max - 1) buf[(*pos)++] = *s++;
}
/* innerHTML getter: serialize an element's children into buf. */
static void dom_serialize(dom_node_t *node, char *buf, int *pos, int max) {
    for (dom_node_t *c = node->first_child; c && *pos < max - 1; c = c->next_sibling) {
        if (c->type == DOM_TEXT) { dom_emit(buf, pos, max, c->text); continue; }
        if (c->style.display == 2 && str_eq(c->tag, "script")) continue;
        dom_emit(buf, pos, max, "<"); dom_emit(buf, pos, max, c->tag);
        if (c->attr_id[0])    { dom_emit(buf, pos, max, " id=\""); dom_emit(buf, pos, max, c->attr_id); dom_emit(buf, pos, max, "\""); }
        if (c->attr_class[0]) { dom_emit(buf, pos, max, " class=\""); dom_emit(buf, pos, max, c->attr_class); dom_emit(buf, pos, max, "\""); }
        dom_emit(buf, pos, max, ">");
        dom_serialize(c, buf, pos, max);
        dom_emit(buf, pos, max, "</"); dom_emit(buf, pos, max, c->tag); dom_emit(buf, pos, max, ">");
    }
}
void dom_get_inner_html(dom_node_t *el, char *buf, int max) {
    int pos = 0;
    if (el && buf && max > 0) dom_serialize(el, buf, &pos, max);
    if (buf && max > 0) buf[pos < max ? pos : max - 1] = 0;
}

/* ── classList ── (operate on the space-separated attr_class) */
int dom_class_contains(dom_node_t *el, const char *cls) {
    return el ? dom_class_has(el->attr_class, cls) : 0;
}
void dom_class_add(dom_node_t *el, const char *cls) {
    if (!el || !cls || !cls[0] || dom_class_has(el->attr_class, cls)) return;
    int len = 0; while (el->attr_class[len]) len++;
    if (len && len < 255) el->attr_class[len++] = ' ';
    for (int i = 0; cls[i] && len < 255; i++) el->attr_class[len++] = cls[i];
    el->attr_class[len] = 0;
    g_dom_dirty = 1;
}
void dom_class_remove(dom_node_t *el, const char *cls) {
    if (!el || !cls) return;
    int cl = 0; while (cls[cl]) cl++;
    char out[256]; int o = 0;
    const char *p = el->attr_class;
    while (*p) {
        while (*p == ' ') p++;
        const char *start = p;
        while (*p && *p != ' ') p++;
        int tlen = (int)(p - start);
        if (tlen == 0) continue;
        int match = (tlen == cl);
        if (match) for (int i = 0; i < tlen; i++) if (start[i] != cls[i]) { match = 0; break; }
        if (!match) {
            if (o && o < 255) out[o++] = ' ';
            for (int i = 0; i < tlen && o < 255; i++) out[o++] = start[i];
        }
    }
    out[o] = 0;
    int i = 0; for (; out[i] && i < 255; i++) el->attr_class[i] = out[i];
    el->attr_class[i] = 0;
    g_dom_dirty = 1;
}
void dom_class_toggle(dom_node_t *el, const char *cls) {
    if (dom_class_contains(el, cls)) dom_class_remove(el, cls);
    else dom_class_add(el, cls);
}

/* Detach an element from its parent (Element.remove / removeChild). */
void dom_remove(dom_node_t *el) {
    if (!el || !el->parent) return;
    dom_node_t *p = el->parent;
    if (p->first_child == el) {
        p->first_child = el->next_sibling;
    } else {
        dom_node_t *s = p->first_child;
        while (s && s->next_sibling != el) s = s->next_sibling;
        if (s) s->next_sibling = el->next_sibling;
    }
    el->parent = 0; el->next_sibling = 0;
    g_dom_dirty = 1;
}

dom_node_t *dom_parent(dom_node_t *el) { return el ? el->parent : 0; }

/* Computed layout box (offsetLeft/Top/Width/Height for JS). */
int dom_box_x(dom_node_t *el) { return el ? el->box.x : 0; }
int dom_box_y(dom_node_t *el) { return el ? el->box.y : 0; }
int dom_box_w(dom_node_t *el) { return el ? el->box.w : 0; }
int dom_box_h(dom_node_t *el) { return el ? el->box.h : 0; }

/* document.body */
dom_node_t *dom_get_body(dom_node_t *root) {
    if (!root) return 0;
    for (dom_node_t *c = root->first_child; c; c = c->next_sibling) {
        if (c->type == DOM_ELEMENT && str_eq(c->tag, "body")) return c;
        dom_node_t *b = dom_get_body(c);
        if (b) return b;
    }
    return 0;
}

const char *dom_get_text_content(dom_node_t *el) {
    if (!el) return "";
    if (el->type == DOM_TEXT) return el->text;
    for (dom_node_t *c = el->first_child; c; c = c->next_sibling)
        if (c->type == DOM_TEXT) return c->text;
    return "";
}

/* textContent = s : replace children with a single text node holding s. */
void dom_set_text_content(dom_node_t *el, const char *s) {
    if (!el || !s) return;
    if (el->type == DOM_TEXT) {
        int i = 0; for (; s[i] && i < 1023; i++) el->text[i] = s[i]; el->text[i] = 0;
        g_dom_dirty = 1;
        return;
    }
    dom_node_t *t = dom_alloc();
    if (!t) return;
    t->type = DOM_TEXT;
    t->parent = el;
    t->style = el->style;         /* inherit font_size/color — css already ran */
    int i = 0; for (; s[i] && i < 1023; i++) t->text[i] = s[i]; t->text[i] = 0;
    el->first_child = t;          /* textContent replaces all children */
    t->next_sibling = 0;
    g_dom_dirty = 1;
}

/* getAttribute/setAttribute over the fixed attribute slots. */
const char *dom_get_attr(dom_node_t *el, const char *name) {
    if (!el || !name) return 0;
    if (str_eq(name, "id"))    return el->attr_id;
    if (str_eq(name, "class")) return el->attr_class;
    if (str_eq(name, "href"))  return el->attr_href;
    if (str_eq(name, "src"))   return el->attr_src;
    if (str_eq(name, "style")) return el->attr_style;
    if (str_eq(name, "value")) return el->attr_value;
    if (str_eq(name, "type"))  return el->attr_type;
    return 0;
}
void dom_set_attr(dom_node_t *el, const char *name, const char *val) {
    if (!el || !name || !val) return;
    char *dst = 0; int cap = 0;
    if (str_eq(name, "id"))    { dst = el->attr_id;    cap = 128; }
    else if (str_eq(name, "class")) { dst = el->attr_class; cap = 256; }
    else if (str_eq(name, "href"))  { dst = el->attr_href;  cap = 512; }
    else if (str_eq(name, "src"))   { dst = el->attr_src;   cap = 512; }
    else if (str_eq(name, "style")) { dst = el->attr_style; cap = 512; }
    else if (str_eq(name, "value")) { dst = el->attr_value; cap = 256; }
    else return;
    int i = 0; for (; val[i] && i < cap - 1; i++) dst[i] = val[i]; dst[i] = 0;
    g_dom_dirty = 1;
}

/* Run all <script> nodes in the tree against the JS engine (qjs_dom.c). */
void browser_run_scripts(dom_node_t *root) {
    extern void zeos_js_run_page(dom_node_t *root);
    zeos_js_run_page(root);
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

            /* <script>: CAPTURE the body so JS can run; <style>: skip (CSS is
             * handled separately). Both stop at their matching close tag. */
            if (str_eq(tag, "script") || str_eq(tag, "style")) {
                int is_script = str_eq(tag, "script");
                while (p < end && *p != '>') p++;   /* past the open tag's attrs */
                if (p < end) p++;
                const char *body = p;
                const char *bend = p;
                while (bend + 1 < end) {
                    if (bend[0] == '<' && bend[1] == '/') {
                        const char *check = bend + 2;
                        char ctag[32];
                        read_tag(&check, end, ctag, 32);
                        if (str_eq(ctag, tag)) break;
                    }
                    bend++;
                }
                if (is_script) {
                    int slen = (int)(bend - body);
                    dom_node_t *snode = dom_alloc();
                    if (snode) {
                        snode->type = DOM_ELEMENT;
                        str_copy(snode->tag, "script");
                        snode->parent = current;
                        snode->style.display = 2;   /* never rendered */
                        if (slen > 0) {
                            snode->script_src = (char *)kmalloc((uint64_t)slen + 1);
                            if (snode->script_src) {
                                for (int i = 0; i < slen; i++) snode->script_src[i] = body[i];
                                snode->script_src[slen] = 0;
                            }
                        }
                        if (!current->first_child) current->first_child = snode;
                        else {
                            dom_node_t *sib = current->first_child;
                            while (sib->next_sibling) sib = sib->next_sibling;
                            sib->next_sibling = snode;
                        }
                    }
                }
                /* advance past the closing tag */
                p = bend;
                while (p < end && *p != '>') p++;
                if (p < end) p++;
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
                    else if (str_eq(attr_name, "type"))
                        read_attr_val(&p, end, node->attr_type, 32);
                    else if (str_eq(attr_name, "value"))
                        read_attr_val(&p, end, node->attr_value, 256);
                    else if (str_eq(attr_name, "alt"))
                        read_attr_val(&p, end, node->attr_alt, 256);
                    else if (str_eq(attr_name, "placeholder"))
                        read_attr_val(&p, end, node->attr_placeholder, 256);
                    else if (str_eq(attr_name, "id"))
                        read_attr_val(&p, end, node->attr_id, 128);
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

/* ── HTML Entity Decoder ──
 * Operates in-place over each text-node buffer. Output is always <= input
 * length: every entity decodes to at most 4 UTF-8 bytes. */

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int utf8_encode(uint32_t cp, char *out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp < 0x110000) {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

static int ent_name_eq(const char *buf, int n, const char *name) {
    int i = 0;
    while (i < n && name[i]) {
        if (buf[i] != name[i]) return 0;
        i++;
    }
    return i == n && name[i] == 0;
}

static uint32_t lookup_named_entity(const char *name, int n) {
    if (ent_name_eq(name, n, "amp"))    return 0x26;
    if (ent_name_eq(name, n, "lt"))     return 0x3C;
    if (ent_name_eq(name, n, "gt"))     return 0x3E;
    if (ent_name_eq(name, n, "quot"))   return 0x22;
    if (ent_name_eq(name, n, "apos"))   return 0x27;
    if (ent_name_eq(name, n, "nbsp"))   return 0xA0;
    if (ent_name_eq(name, n, "copy"))   return 0xA9;
    if (ent_name_eq(name, n, "reg"))    return 0xAE;
    if (ent_name_eq(name, n, "trade"))  return 0x2122;
    if (ent_name_eq(name, n, "hellip")) return 0x2026;
    if (ent_name_eq(name, n, "mdash"))  return 0x2014;
    if (ent_name_eq(name, n, "ndash"))  return 0x2013;
    if (ent_name_eq(name, n, "lsquo"))  return 0x2018;
    if (ent_name_eq(name, n, "rsquo"))  return 0x2019;
    if (ent_name_eq(name, n, "ldquo"))  return 0x201C;
    if (ent_name_eq(name, n, "rdquo"))  return 0x201D;
    return 0;
}

static void decode_entities_str(char *s) {
    const char *r = s;
    char *w = s;
    while (*r) {
        if (*r != '&') { *w++ = *r++; continue; }
        const char *semi = 0;
        for (int i = 1; i <= 10 && r[i]; i++) {
            char c = r[i];
            if (c == ';') { semi = r + i; break; }
            if (c == '&' || c == ' ' || c == '<' || c == '>') break;
        }
        if (!semi) { *w++ = *r++; continue; }

        uint32_t cp = 0;
        int matched = 0;

        if (r[1] == '#') {
            if (r[2] == 'x' || r[2] == 'X') {
                long v = 0; int ok = (semi > r + 3);
                for (const char *p = r + 3; ok && p < semi; p++) {
                    int d = hex_val(*p);
                    if (d < 0) { ok = 0; break; }
                    v = (v << 4) | d;
                    if (v > 0x10FFFF) { ok = 0; break; }
                }
                if (ok) { cp = (uint32_t)v; matched = 1; }
            } else {
                long v = 0; int ok = (semi > r + 2);
                for (const char *p = r + 2; ok && p < semi; p++) {
                    if (*p < '0' || *p > '9') { ok = 0; break; }
                    v = v * 10 + (*p - '0');
                    if (v > 0x10FFFF) { ok = 0; break; }
                }
                if (ok) { cp = (uint32_t)v; matched = 1; }
            }
        } else {
            int nlen = (int)(semi - (r + 1));
            cp = lookup_named_entity(r + 1, nlen);
            if (cp) matched = 1;
        }

        if (matched && cp != 0) {
            char tmp[4];
            int n = utf8_encode(cp, tmp);
            for (int i = 0; i < n; i++) *w++ = tmp[i];
            r = semi + 1;
        } else {
            *w++ = *r++;
        }
    }
    *w = 0;
}

/* Walk the DOM, decoding entities in every text node. */
static void decode_entities(dom_node_t *node) {
    if (!node) return;
    if (node->type == DOM_TEXT) decode_entities_str(node->text);
    for (dom_node_t *c = node->first_child; c; c = c->next_sibling)
        decode_entities(c);
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
    node->style.flex_dir = 0; node->style.justify = 0;
    node->style.align = 0;    node->style.gap = 0;
    node->style.flex_grow = 0; node->style.grid_ncols = 0;
    for (int i = 0; i < 8; i++) node->style.grid_track[i] = 0;

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
    } else if (str_eq(node->tag, "input")) {
        node->style.display = 1;  /* inline (inline-block equivalent) */
        node->style.margin[1] = 4; node->style.margin[3] = 4;
        node->style.padding[0] = 4; node->style.padding[1] = 8;
        node->style.padding[2] = 4; node->style.padding[3] = 8;
    } else if (str_eq(node->tag, "button")) {
        node->style.display = 1;  /* inline */
        node->style.font_weight = 700;
        node->style.background = COLOR_PRIMARY;
        node->style.color = COLOR_SURFACE;
        node->style.margin[1] = 4; node->style.margin[3] = 4;
        node->style.padding[0] = 6; node->style.padding[1] = 16;
        node->style.padding[2] = 6; node->style.padding[3] = 16;
    } else if (str_eq(node->tag, "textarea")) {
        node->style.display = 0;  /* block */
        node->style.margin[0] = 4; node->style.margin[2] = 4;
        node->style.padding[0] = 6; node->style.padding[1] = 8;
        node->style.padding[2] = 6; node->style.padding[3] = 8;
    } else if (str_eq(node->tag, "img")) {
        node->style.display = 0;  /* block */
        node->style.margin[0] = 4; node->style.margin[2] = 4;
    } else if (str_eq(node->tag, "head") || str_eq(node->tag, "meta") ||
               str_eq(node->tag, "title") || str_eq(node->tag, "link")) {
        node->style.display = 2;  /* none — hide head elements */
    }

    /* Recurse */
    for (dom_node_t *c = node->first_child; c; c = c->next_sibling)
        css_apply_defaults(c);
}

/* ── CSS inline style parser ── */

/* Parse a hex digit: '0'-'9','a'-'f','A'-'F' → 0-15, else -1 */
static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parse #RRGGBB or #RGB into 0xFFRRGGBB. Returns 0 on failure. */
static uint32_t parse_hex_color(const char *s) {
    if (*s != '#') return 0;
    s++;

    int digits = 0;
    const char *t = s;
    while (hex_digit(*t) >= 0) { digits++; t++; }

    if (digits == 6) {
        int r = (hex_digit(s[0]) << 4) | hex_digit(s[1]);
        int g = (hex_digit(s[2]) << 4) | hex_digit(s[3]);
        int b = (hex_digit(s[4]) << 4) | hex_digit(s[5]);
        return 0xFF000000 | (r << 16) | (g << 8) | b;
    } else if (digits == 3) {
        int r = hex_digit(s[0]); r = (r << 4) | r;
        int g = hex_digit(s[1]); g = (g << 4) | g;
        int b = hex_digit(s[2]); b = (b << 4) | b;
        return 0xFF000000 | (r << 16) | (g << 8) | b;
    }
    return 0;
}

/* Parse integer from string, return value and advance *p past digits */
static int parse_int(const char **p) {
    int val = 0;
    while (**p >= '0' && **p <= '9') {
        val = val * 10 + (**p - '0');
        (*p)++;
    }
    return val;
}

/* Skip whitespace in style string */
static void css_skip_ws(const char **p) {
    while (**p == ' ' || **p == '\t') (*p)++;
}

/* Compare a CSS property name (lowercase, up to ':' or end) */
static int css_prop_eq(const char *prop, const char *name) {
    while (*name) {
        if (*prop != *name) return 0;
        prop++; name++;
    }
    return 1;
}

/* Parse a single CSS property:value pair and apply to node */
static void css_apply_property(dom_node_t *node, const char *prop,
                               int prop_len, const char *val, int val_len)
{
    /* Null-terminate copies for parsing (on stack, small) */
    char pbuf[64], vbuf[128];
    int pi = 0, vi = 0;
    for (int i = 0; i < prop_len && pi < 63; i++)
        pbuf[pi++] = to_lower(prop[i]);
    pbuf[pi] = 0;
    for (int i = 0; i < val_len && vi < 127; i++)
        vbuf[vi++] = val[i];
    vbuf[vi] = 0;

    /* Trim leading/trailing spaces from value */
    const char *v = vbuf;
    while (*v == ' ') v++;
    int vl = str_len(v);
    while (vl > 0 && v[vl - 1] == ' ') vl--;

    /* color: #RRGGBB */
    if (css_prop_eq(pbuf, "color")) {
        uint32_t c = parse_hex_color(v);
        if (c) node->style.color = c;
        return;
    }

    /* background-color: #RRGGBB  or  background: #RRGGBB */
    if (css_prop_eq(pbuf, "background-color") || css_prop_eq(pbuf, "background")) {
        uint32_t c = parse_hex_color(v);
        if (c) node->style.background = c;
        return;
    }

    /* font-size: Npx */
    if (css_prop_eq(pbuf, "font-size")) {
        const char *vp = v;
        int sz = parse_int(&vp);
        if (sz > 0) node->style.font_size = sz;
        return;
    }

    /* font-weight: bold|700|normal|400 */
    if (css_prop_eq(pbuf, "font-weight")) {
        if (str_starts(v, "bold") || str_starts(v, "700"))
            node->style.font_weight = 700;
        else if (str_starts(v, "normal") || str_starts(v, "400"))
            node->style.font_weight = 400;
        return;
    }

    /* text-align: left|center|right */
    if (css_prop_eq(pbuf, "text-align")) {
        if (str_starts(v, "left"))   node->style.text_align = 0;
        if (str_starts(v, "center")) node->style.text_align = 1;
        if (str_starts(v, "right"))  node->style.text_align = 2;
        return;
    }

    /* margin: Npx (single value = all four sides) */
    if (css_prop_eq(pbuf, "margin")) {
        const char *vp = v;
        int m = parse_int(&vp);
        if (m >= 0) {
            node->style.margin[0] = m; node->style.margin[1] = m;
            node->style.margin[2] = m; node->style.margin[3] = m;
        }
        return;
    }

    /* padding: Npx (single value = all four sides) */
    if (css_prop_eq(pbuf, "padding")) {
        const char *vp = v;
        int p = parse_int(&vp);
        if (p >= 0) {
            node->style.padding[0] = p; node->style.padding[1] = p;
            node->style.padding[2] = p; node->style.padding[3] = p;
        }
        return;
    }

    /* display: none|block|inline|flex|grid */
    if (css_prop_eq(pbuf, "display")) {
        if (str_starts(v, "grid"))        node->style.display = 4;
        else if (str_starts(v, "flex"))   node->style.display = 3;
        else if (str_starts(v, "block"))  node->style.display = 0;
        else if (str_starts(v, "inline")) node->style.display = 1;
        else if (str_starts(v, "none"))   node->style.display = 2;
        return;
    }

    /* grid-template-columns: repeat(N, <track>) | <track> <track> ...
     * track = "Npx"/"N" (fixed) or "Nfr"/"fr"/"auto" (flexible → grid_track=0). */
    if (css_prop_eq(pbuf, "grid-template-columns")) {
        int nc = 0;
        const char *p = v;
        if (str_starts(p, "repeat(")) {
            p += 7;
            int count = parse_int(&p);          /* repeat(N, ...) */
            while (*p && *p != ',') p++;
            if (*p == ',') p++;
            while (*p == ' ') p++;
            int px = parse_int(&p);              /* track size (0 if "1fr"/"auto") */
            int is_px = 0;
            for (const char *q = p; *q && *q != ')'; q++)
                if (*q == 'p' && *(q+1) == 'x') is_px = 1;
            if (count > 8) count = 8;
            for (int i = 0; i < count; i++)
                node->style.grid_track[i] = is_px ? px : 0;
            nc = count;
        } else {
            /* explicit track list separated by spaces */
            while (*p && nc < 8) {
                while (*p == ' ') p++;
                if (!*p) break;
                int val = parse_int(&p);
                int is_px = (p[0] == 'p' && p[1] == 'x');
                node->style.grid_track[nc++] = is_px ? val : 0;  /* fr/auto → flexible */
                while (*p && *p != ' ') p++;    /* skip unit */
            }
        }
        node->style.grid_ncols = nc;
        return;
    }

    /* flex-direction: row|column */
    if (css_prop_eq(pbuf, "flex-direction")) {
        node->style.flex_dir = str_starts(v, "column") ? 1 : 0;
        return;
    }

    /* justify-content: main-axis distribution */
    if (css_prop_eq(pbuf, "justify-content")) {
        if      (str_starts(v, "space-between")) node->style.justify = 3;
        else if (str_starts(v, "space-around"))  node->style.justify = 4;
        else if (str_starts(v, "center"))        node->style.justify = 1;
        else if (str_starts(v, "flex-end") || str_starts(v, "end")) node->style.justify = 2;
        else                                     node->style.justify = 0; /* start */
        return;
    }

    /* align-items: cross-axis alignment */
    if (css_prop_eq(pbuf, "align-items")) {
        if      (str_starts(v, "center"))   node->style.align = 2;
        else if (str_starts(v, "flex-end") || str_starts(v, "end")) node->style.align = 3;
        else if (str_starts(v, "flex-start") || str_starts(v, "start")) node->style.align = 1;
        else                                node->style.align = 0; /* stretch */
        return;
    }

    /* gap: Npx (between flex/grid items) */
    if (css_prop_eq(pbuf, "gap") || css_prop_eq(pbuf, "grid-gap")) {
        const char *vp = v; int g = parse_int(&vp);
        if (g >= 0) node->style.gap = g;
        return;
    }

    /* flex / flex-grow: N (grow factor; flex:1 => grow to fill) */
    if (css_prop_eq(pbuf, "flex") || css_prop_eq(pbuf, "flex-grow")) {
        const char *vp = v; int g = parse_int(&vp);
        if (g >= 0) node->style.flex_grow = g;
        return;
    }

    /* Unknown property — skip gracefully */
}

void css_apply_inline(dom_node_t *node) {
    if (!node) return;

    /* Parse style="" if present */
    if (node->attr_style[0]) {
        const char *p = node->attr_style;
        while (*p) {
            /* Skip whitespace and semicolons */
            while (*p == ' ' || *p == ';' || *p == '\t') p++;
            if (!*p) break;

            /* Read property name (up to ':') */
            const char *prop_start = p;
            while (*p && *p != ':' && *p != ';') p++;
            int prop_len = (int)(p - prop_start);

            /* Trim trailing spaces from property */
            while (prop_len > 0 && prop_start[prop_len - 1] == ' ')
                prop_len--;

            if (*p != ':') continue;  /* Malformed — skip */
            p++;  /* skip ':' */

            /* Read value (up to ';' or end) */
            while (*p == ' ') p++;
            const char *val_start = p;
            while (*p && *p != ';') p++;
            int val_len = (int)(p - val_start);

            /* Trim trailing spaces from value */
            while (val_len > 0 && val_start[val_len - 1] == ' ')
                val_len--;

            if (prop_len > 0 && val_len > 0)
                css_apply_property(node, prop_start, prop_len,
                                   val_start, val_len);

            if (*p == ';') p++;
        }
    }

    /* Recurse into children */
    for (dom_node_t *c = node->first_child; c; c = c->next_sibling)
        css_apply_inline(c);
}

/* ── Layout helpers ── */

/* Determine the font_id for a DOM node based on its tag */
static font_id_t node_font(dom_node_t *n) {
    if (str_eq(n->tag, "code") || str_eq(n->tag, "pre"))
        return FONT_CODE;
    if (n->style.font_weight >= 700)
        return FONT_UI_BOLD;
    return FONT_UI;
}

/* Walk parents to inherit font id (e.g. text inside <code>) */
static font_id_t text_font(dom_node_t *text_node) {
    for (dom_node_t *p = text_node->parent; p; p = p->parent) {
        if (str_eq(p->tag, "code") || str_eq(p->tag, "pre"))
            return FONT_CODE;
        if (p->style.font_weight >= 700)
            return FONT_UI_BOLD;
    }
    return FONT_UI;
}

/* ── Layout Engine ── */

/* Shrink-to-fit intrinsic content width (border-box: includes the node's own
 * L/R padding, excludes its margins). Used to size flex items on the main axis. */
static int intrinsic_w(dom_node_t *n, int avail) {
    if (!n || n->style.display == 2) return 0;
    int pad_lr = n->style.padding[1] + n->style.padding[3];
    if (n->type == DOM_TEXT)
        return font_measure(n->text, text_font(n), n->style.font_size) + pad_lr;
    if (str_eq(n->tag, "img"))
        return ((n->img_pixels && n->img_w > 0) ? n->img_w : 200) + pad_lr;
    if (str_eq(n->tag, "input") || str_eq(n->tag, "textarea"))
        return 200 + pad_lr;
    if (str_eq(n->tag, "button")) {
        const char *label = n->attr_value[0] ? n->attr_value :
            ((n->first_child && n->first_child->type == DOM_TEXT) ? n->first_child->text : "");
        return font_measure(label, FONT_UI_BOLD, TYPE_BODY) + pad_lr;
    }
    /* Container: flex-row => sum of children; otherwise widest child. */
    int row = (n->style.display == 3 && n->style.flex_dir == 0);
    int total = 0, maxw = 0, cnt = 0;
    for (dom_node_t *c = n->first_child; c; c = c->next_sibling) {
        if (c->style.display == 2) continue;
        int cw = intrinsic_w(c, avail) + c->style.margin[1] + c->style.margin[3];
        total += cw;
        if (cw > maxw) maxw = cw;
        cnt++;
    }
    int inner = row ? (total + (cnt > 1 ? n->style.gap * (cnt - 1) : 0)) : maxw;
    int w = inner + pad_lr;
    if (avail > 0 && w > avail) w = avail;
    return w;
}

/* CSS flexbox: lay out `box`'s children along the main axis (row or column),
 * distribute free space per justify-content, align on the cross axis per
 * align-items, honor gap and per-item flex-grow. Standard single-line flex. */
static void layout_flex(dom_node_t *box, int content_w, int content_h) {
    int row = (box->style.flex_dir == 0);
    int gap = box->style.gap;

    /* Pass 1: measure each visible child's width + height. */
    int n = 0, used = 0, grow_total = 0, max_cross = 0;
    for (dom_node_t *c = box->first_child; c; c = c->next_sibling) {
        if (c->style.display == 2) continue;
        int cw = intrinsic_w(c, content_w), ch;
        if (c->type == DOM_TEXT) {
            ch = font_line_height(text_font(c), c->style.font_size);
            if (ch < 1) ch = c->style.font_size * 3 / 2;
        } else if (str_eq(c->tag, "img")) {
            ch = (c->img_pixels && c->img_h > 0) ? c->img_h : 80;
        } else if (str_eq(c->tag, "input") || str_eq(c->tag, "button") || str_eq(c->tag, "textarea")) {
            font_id_t fid = str_eq(c->tag, "button") ? FONT_UI_BOLD : FONT_UI;
            int lh = font_line_height(fid, TYPE_BODY); if (lh < 1) lh = TYPE_BODY * 3 / 2;
            ch = lh + c->style.padding[0] + c->style.padding[2];
            if (str_eq(c->tag, "textarea")) ch = lh * 4 + c->style.padding[0] + c->style.padding[2];
        } else {
            layout_compute(c, cw - c->style.padding[1] - c->style.padding[3], content_h);
            int inner_h = 0;
            for (dom_node_t *gc = c->first_child; gc; gc = gc->next_sibling) {
                if (gc->style.display == 2) continue;
                int b = gc->box.y + gc->box.h + gc->style.margin[2];
                if (b > inner_h) inner_h = b;
            }
            ch = inner_h + c->style.padding[0] + c->style.padding[2];
        }
        c->box.w = cw; c->box.h = ch;
        used += (row ? cw + c->style.margin[1] + c->style.margin[3]
                     : ch + c->style.margin[0] + c->style.margin[2]);
        grow_total += c->style.flex_grow;
        int cross = row ? ch + c->style.margin[0] + c->style.margin[2]
                        : cw + c->style.margin[1] + c->style.margin[3];
        if (cross > max_cross) max_cross = cross;
        n++;
    }
    if (n == 0) return;
    used += gap * (n - 1);

    /* Row cross = tallest item; column cross = available width. */
    int cross_size = row ? max_cross : content_w;
    /* Row main = container width; column main = auto (content) => no free space. */
    int main_size = row ? content_w : used;
    int free = main_size - used;
    if (free < 0) free = 0;

    int cursor = 0, between = gap;
    if (grow_total == 0) {
        switch (box->style.justify) {
            case 1: cursor = free / 2; break;                              /* center */
            case 2: cursor = free; break;                                  /* end */
            case 3: if (n > 1) between = gap + free / (n - 1); break;      /* space-between */
            case 4: cursor = free / (2 * n); between = gap + free / n; break; /* space-around */
            default: break;                                               /* flex-start */
        }
    }

    /* Pass 2: position along main axis, align on cross axis. */
    for (dom_node_t *c = box->first_child; c; c = c->next_sibling) {
        if (c->style.display == 2) continue;
        int grow_extra = (grow_total > 0) ? (free * c->style.flex_grow / grow_total) : 0;
        if (row) {
            c->box.w += grow_extra;
            c->box.x = cursor + c->style.margin[3];
            int ext = c->box.h + c->style.margin[0] + c->style.margin[2], cy;
            switch (box->style.align) {
                case 2: cy = (cross_size - ext) / 2; break;                /* center */
                case 3: cy = cross_size - ext; break;                      /* flex-end */
                case 0: c->box.h = cross_size - c->style.margin[0] - c->style.margin[2]; cy = 0; break; /* stretch */
                default: cy = 0; break;                                    /* flex-start */
            }
            if (cy < 0) cy = 0;
            c->box.y = cy + c->style.margin[0];
            cursor += c->box.w + c->style.margin[1] + c->style.margin[3] + between;
        } else {
            c->box.h += grow_extra;
            c->box.y = cursor + c->style.margin[0];
            int ext = c->box.w + c->style.margin[1] + c->style.margin[3], cx;
            switch (box->style.align) {
                case 2: cx = (cross_size - ext) / 2; break;
                case 3: cx = cross_size - ext; break;
                case 0: c->box.w = cross_size - c->style.margin[1] - c->style.margin[3]; cx = 0; break;
                default: cx = 0; break;
            }
            if (cx < 0) cx = 0;
            c->box.x = cx + c->style.margin[3];
            cursor += c->box.h + c->style.margin[0] + c->style.margin[2] + between;
        }
    }
}

/* CSS grid: fixed-column-count, row-major auto-placement. Column widths from
 * grid-template-columns (fixed px tracks + flexible 1fr tracks share the rest);
 * each grid row's height = tallest item in that row. Honors gap. */
static void layout_grid(dom_node_t *box, int content_w, int content_h) {
    int nc = box->style.grid_ncols;
    if (nc < 1) nc = 1;
    if (nc > 8) nc = 8;
    int gap = box->style.gap;

    /* Resolve column pixel widths: fixed tracks keep their px, flexible tracks
     * split the remaining width equally. */
    int fixed_sum = 0, fr_count = 0;
    for (int i = 0; i < nc; i++) {
        if (box->style.grid_track[i] > 0) fixed_sum += box->style.grid_track[i];
        else fr_count++;
    }
    int avail = content_w - fixed_sum - gap * (nc - 1);
    if (avail < 0) avail = 0;
    int fr_w = fr_count > 0 ? avail / fr_count : 0;
    int col_w[8], col_x[8], cx = 0;
    for (int i = 0; i < nc; i++) {
        col_w[i] = box->style.grid_track[i] > 0 ? box->style.grid_track[i] : fr_w;
        col_x[i] = cx;
        cx += col_w[i] + gap;
    }

    /* Place items row-major; a row's height = tallest item, then advance y. */
    int idx = 0, row_y = 0, row_h = 0;
    for (dom_node_t *c = box->first_child; c; c = c->next_sibling) {
        if (c->style.display == 2) continue;
        int col = idx % nc;
        int cw = col_w[col];

        /* Size the item to its column width, measure its height. */
        int ch;
        if (c->type == DOM_TEXT) {
            ch = font_line_height(text_font(c), c->style.font_size);
            if (ch < 1) ch = c->style.font_size * 3 / 2;
        } else if (str_eq(c->tag, "img")) {
            ch = (c->img_pixels && c->img_h > 0) ? c->img_h : 80;
        } else if (str_eq(c->tag, "input") || str_eq(c->tag, "button") || str_eq(c->tag, "textarea")) {
            font_id_t fid = str_eq(c->tag, "button") ? FONT_UI_BOLD : FONT_UI;
            int lh = font_line_height(fid, TYPE_BODY); if (lh < 1) lh = TYPE_BODY * 3 / 2;
            ch = lh + c->style.padding[0] + c->style.padding[2];
            if (str_eq(c->tag, "textarea")) ch = lh * 4 + c->style.padding[0] + c->style.padding[2];
        } else {
            layout_compute(c, cw - c->style.padding[1] - c->style.padding[3], content_h);
            int inner_h = 0;
            for (dom_node_t *gc = c->first_child; gc; gc = gc->next_sibling) {
                if (gc->style.display == 2) continue;
                int b = gc->box.y + gc->box.h + gc->style.margin[2];
                if (b > inner_h) inner_h = b;
            }
            ch = inner_h + c->style.padding[0] + c->style.padding[2];
        }
        c->box.w = cw;
        c->box.h = ch;
        c->box.x = col_x[col];
        c->box.y = row_y;
        if (ch > row_h) row_h = ch;

        idx++;
        if (idx % nc == 0) { row_y += row_h + gap; row_h = 0; }  /* end of row */
    }
}

void layout_compute(dom_node_t *root, int viewport_w, int viewport_h) {
    if (!root) return;
    if (root->style.display == 3) { layout_flex(root, viewport_w, viewport_h); return; }
    if (root->style.display == 4) { layout_grid(root, viewport_w, viewport_h); return; }
    (void)viewport_h;

    /* Simple block layout: stack blocks vertically, inline flows horizontally */
    int cursor_y = 0;

    for (dom_node_t *c = root->first_child; c; c = c->next_sibling) {
        if (c->style.display == 2) continue;  /* display:none */

        c->box.x = c->style.margin[3] + c->style.padding[3];
        c->box.y = cursor_y + c->style.margin[0];
        c->box.w = viewport_w - c->style.margin[1] - c->style.margin[3];

        if (c->type == DOM_TEXT) {
            /* Use font_measure for accurate text width */
            font_id_t fid = text_font(c);
            int size_px = c->style.font_size;
            int text_w = font_measure(c->text, fid, size_px);
            int line_h = font_line_height(fid, size_px);
            if (line_h < 1) line_h = size_px * 3 / 2;

            /* Word-wrap: how many visual lines? */
            if (text_w <= c->box.w || c->box.w <= 0) {
                c->box.h = line_h;
            } else {
                /* Estimate lines from total width vs available width */
                int lines = (text_w + c->box.w - 1) / c->box.w;
                if (lines < 1) lines = 1;
                c->box.h = lines * line_h;
            }
        } else if (str_eq(c->tag, "img")) {
            /* If we already decoded the PNG, use real dimensions.
             * Otherwise fall back to a 200x80 placeholder slot. */
            int iw = (c->img_pixels && c->img_w > 0) ? c->img_w : 200;
            int ih = (c->img_pixels && c->img_h > 0) ? c->img_h : 80;
            /* Clamp to viewport width so huge images don't blow layout */
            int avail = viewport_w - c->style.padding[1] - c->style.padding[3];
            if (avail > 0 && iw > avail) {
                /* Scale height proportionally so aspect ratio survives. */
                if (c->img_pixels && c->img_w > 0) {
                    ih = (ih * avail) / iw;
                }
                iw = avail;
            }
            c->box.w = iw + c->style.padding[1] + c->style.padding[3];
            c->box.h = ih + c->style.padding[0] + c->style.padding[2];
        } else if (str_eq(c->tag, "input")) {
            /* Text input: fixed height, width fills container or 200px min */
            int line_h = font_line_height(FONT_UI, TYPE_BODY);
            if (line_h < 1) line_h = TYPE_BODY * 3 / 2;
            c->box.h = line_h + c->style.padding[0] + c->style.padding[2];
            if (c->box.w < 200) c->box.w = 200;
        } else if (str_eq(c->tag, "button")) {
            /* Button: sized to text content */
            const char *label = "";
            if (c->attr_value[0])
                label = c->attr_value;
            else if (c->first_child && c->first_child->type == DOM_TEXT)
                label = c->first_child->text;
            int tw = font_measure(label, FONT_UI_BOLD, TYPE_BODY);
            int line_h = font_line_height(FONT_UI_BOLD, TYPE_BODY);
            if (line_h < 1) line_h = TYPE_BODY * 3 / 2;
            c->box.w = tw + c->style.padding[1] + c->style.padding[3];
            c->box.h = line_h + c->style.padding[0] + c->style.padding[2];
        } else if (str_eq(c->tag, "textarea")) {
            /* Textarea: full width, minimum 4 lines tall */
            int line_h = font_line_height(FONT_UI, TYPE_BODY);
            if (line_h < 1) line_h = TYPE_BODY * 3 / 2;
            int min_h = line_h * 4 + c->style.padding[0] + c->style.padding[2];
            c->box.h = min_h;
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

/* ── PNG placeholder ── */

/*
 * Minimal PNG IHDR parser — extracts width and height from a PNG.
 * A full PNG decode requires inflate/deflate (zlib), which is too heavy
 * for Alpha 0.1. Instead we parse the image dimensions from IHDR and
 * render an accent-tinted placeholder rectangle with "IMG" text.
 *
 * Returns 1 if valid PNG header found, 0 otherwise.
 * Sets *w and *h from IHDR chunk.
 */
static int __attribute__((unused))
png_parse_ihdr(const uint8_t *data, int len, int *w, int *h) {
    /* PNG signature: 89 50 4E 47 0D 0A 1A 0A */
    if (len < 24) return 0;
    if (data[0] != 0x89 || data[1] != 0x50 || data[2] != 0x4E ||
        data[3] != 0x47 || data[4] != 0x0D || data[5] != 0x0A ||
        data[6] != 0x1A || data[7] != 0x0A) return 0;

    /* IHDR must be the first chunk (offset 8).
     * Chunk layout: 4 bytes length, 4 bytes type, data, 4 bytes CRC.
     * IHDR type = "IHDR" = 0x49484452.
     * IHDR data: 4 bytes width, 4 bytes height, 1 bit depth, 1 color type,
     *            1 compression, 1 filter, 1 interlace. */
    if (data[12] != 'I' || data[13] != 'H' || data[14] != 'D' ||
        data[15] != 'R') return 0;

    /* Width and height are big-endian 32-bit at offsets 16 and 20 */
    *w = (data[16] << 24) | (data[17] << 16) | (data[18] << 8) | data[19];
    *h = (data[20] << 24) | (data[21] << 16) | (data[22] << 8) | data[23];

    /* Sanity bounds */
    if (*w <= 0 || *w > 4096 || *h <= 0 || *h > 4096) return 0;
    return 1;
}

/*
 * Draw an image placeholder: accent-tinted rect with "IMG" label and
 * dimension text. Used when we have an <img> tag but no decoded pixels.
 *
 * img_w, img_h: actual image dimensions (0 if unknown).
 * draw_w, draw_h: layout box dimensions to fill.
 */
static void render_img_placeholder(int x, int y, int draw_w, int draw_h,
                                   int img_w, int img_h, const char *alt)
{
    /* Accent-tinted background (primary at ~20% opacity) */
    uint32_t bg = COLOR_SURFACE_HIGH;  /* persona-neutral elevated rect */
    fb_rect(x, y, draw_w, draw_h, bg);
    fb_rect_outline(x, y, draw_w, draw_h, COLOR_PRIMARY_DIM, 1);

    /* "IMG" label centered */
    int label_w = font_measure("IMG", FONT_UI_BOLD, TYPE_LABEL);
    int lx = x + (draw_w - label_w) / 2;
    int ly = y + draw_h / 2 - TYPE_LABEL;
    font_draw(lx, ly, "IMG", FONT_UI_BOLD, TYPE_LABEL, COLOR_PRIMARY);

    /* Show alt text or dimensions below the label */
    char info[128];
    int ii = 0;
    if (alt && alt[0]) {
        while (alt[ii] && ii < 80) { info[ii] = alt[ii]; ii++; }
    } else if (img_w > 0 && img_h > 0) {
        /* Build "WxH" string manually */
        int tmp = img_w;
        char digits[12]; int di = 0;
        if (tmp == 0) digits[di++] = '0';
        while (tmp > 0) { digits[di++] = '0' + (tmp % 10); tmp /= 10; }
        for (int j = di - 1; j >= 0; j--) info[ii++] = digits[j];
        info[ii++] = 'x';
        tmp = img_h; di = 0;
        if (tmp == 0) digits[di++] = '0';
        while (tmp > 0) { digits[di++] = '0' + (tmp % 10); tmp /= 10; }
        for (int j = di - 1; j >= 0; j--) info[ii++] = digits[j];
    }
    info[ii] = 0;

    if (ii > 0) {
        int info_w = font_measure(info, FONT_UI, TYPE_CAPTION);
        int ix = x + (draw_w - info_w) / 2;
        int iy = ly + TYPE_LABEL + 4;
        if (iy + TYPE_CAPTION < y + draw_h)
            font_draw(ix, iy, info, FONT_UI, TYPE_CAPTION, COLOR_ON_SURFACE_3);
    }
}

/* ── Form element rendering ── */

/*
 * Render an <input> element: outlined text box with placeholder/value text.
 * Visual only for Alpha — no text input yet.
 */
static void render_input(dom_node_t *node, int x, int y, int w, int h) {
    /* Input field background */
    fb_rect(x, y, w, h, COLOR_SURFACE_TOP);
    fb_rect_outline(x, y, w, h, COLOR_ON_SURFACE_4, 1);

    /* Show value or placeholder text */
    const char *text = node->attr_value[0] ? node->attr_value :
                       node->attr_placeholder[0] ? node->attr_placeholder : "";
    uint32_t text_color = node->attr_value[0] ? COLOR_ON_SURFACE : COLOR_ON_SURFACE_3;

    if (text[0]) {
        int tx = x + node->style.padding[3];
        int ty = y + node->style.padding[0];
        font_draw(tx, ty, text, FONT_UI, TYPE_BODY, text_color);
    }
}

/*
 * Render a <button> element: filled rect with centered text.
 * Accent-colored background, inverted text.
 */
static void render_button(dom_node_t *node, int x, int y, int w, int h) {
    /* Button background (use node's computed background, or primary) */
    uint32_t bg = node->style.background ? node->style.background : COLOR_PRIMARY;
    fb_rect(x, y, w, h, bg);

    /* Slight rounded appearance: darken the 4 corner pixels */
    fb_pixel(x, y, COLOR_SURFACE);
    fb_pixel(x + w - 1, y, COLOR_SURFACE);
    fb_pixel(x, y + h - 1, COLOR_SURFACE);
    fb_pixel(x + w - 1, y + h - 1, COLOR_SURFACE);

    /* Button text: value attr, or first text child */
    const char *label = "";
    if (node->attr_value[0]) {
        label = node->attr_value;
    } else if (node->first_child && node->first_child->type == DOM_TEXT) {
        label = node->first_child->text;
    }

    if (label[0]) {
        uint32_t tc = node->style.color ? node->style.color : COLOR_SURFACE;
        int label_w = font_measure(label, FONT_UI_BOLD, TYPE_BODY);
        int tx = x + (w - label_w) / 2;
        int ty = y + node->style.padding[0];
        font_draw(tx, ty, label, FONT_UI_BOLD, TYPE_BODY, tc);
    }
}

/*
 * Render a <textarea> element: multi-line outlined text box.
 * Visual only for Alpha.
 */
static void render_textarea(dom_node_t *node, int x, int y, int w, int h) {
    /* Minimum height for textarea: 4 lines */
    int min_h = TYPE_BODY * 6;
    if (h < min_h) h = min_h;

    fb_rect(x, y, w, h, COLOR_SURFACE_TOP);
    fb_rect_outline(x, y, w, h, COLOR_ON_SURFACE_4, 1);

    /* Show placeholder or value text */
    const char *text = node->attr_value[0] ? node->attr_value :
                       node->attr_placeholder[0] ? node->attr_placeholder : "";
    uint32_t text_color = node->attr_value[0] ? COLOR_ON_SURFACE : COLOR_ON_SURFACE_3;

    if (text[0]) {
        int tx = x + node->style.padding[3];
        int ty = y + node->style.padding[0];
        font_draw(tx, ty, text, FONT_UI, TYPE_BODY, text_color);
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
        if (c->style.background && !str_eq(c->tag, "button")) {
            fb_rect(draw_x, draw_y, c->box.w, c->box.h, c->style.background);
        }

        /* HR = horizontal line */
        if (str_eq(c->tag, "hr")) {
            fb_hline(draw_x, draw_y + c->box.h / 2, c->box.w, COLOR_SEPARATOR);
            continue;
        }

        /* <img> — blit decoded RGBA pixels if we have them, else placeholder */
        if (str_eq(c->tag, "img")) {
            if (c->img_pixels && c->img_w > 0 && c->img_h > 0) {
                /* Nearest-neighbor scale from native dims to layout box.
                 * lodepng_decode32 returns RGBA, row-major, 8 bits/channel. */
                int dw = c->box.w - c->style.padding[1] - c->style.padding[3];
                int dh = c->box.h - c->style.padding[0] - c->style.padding[2];
                if (dw < 1) dw = c->img_w;
                if (dh < 1) dh = c->img_h;
                int dx0 = draw_x + c->style.padding[3];
                int dy0 = draw_y + c->style.padding[0];

                for (int y = 0; y < dh; y++) {
                    int sy = (y * c->img_h) / dh;
                    if (sy < 0) sy = 0;
                    if (sy >= c->img_h) sy = c->img_h - 1;
                    const uint8_t *row = c->img_pixels + sy * c->img_w * 4;
                    for (int x = 0; x < dw; x++) {
                        int sx = (x * c->img_w) / dw;
                        if (sx < 0) sx = 0;
                        if (sx >= c->img_w) sx = c->img_w - 1;
                        const uint8_t *p = row + sx * 4;
                        uint32_t argb = ((uint32_t)p[3] << 24) |
                                        ((uint32_t)p[0] << 16) |
                                        ((uint32_t)p[1] <<  8) |
                                        ((uint32_t)p[2]);
                        fb_pixel_blend(dx0 + x, dy0 + y, argb);
                    }
                }
            } else {
                int ph_w = c->box.w > 0 ? c->box.w : 200;
                int ph_h = c->box.h > 0 ? c->box.h : 80;
                render_img_placeholder(draw_x, draw_y, ph_w, ph_h,
                                       c->img_w, c->img_h, c->attr_alt);
            }
            continue;
        }

        /* <input> — render text field */
        if (str_eq(c->tag, "input")) {
            int iw = c->box.w > 0 ? c->box.w : 200;
            int ih = c->box.h > 0 ? c->box.h : TYPE_BODY + 12;
            render_input(c, draw_x, draw_y, iw, ih);
            continue;
        }

        /* <button> — render button */
        if (str_eq(c->tag, "button")) {
            int bw = c->box.w > 0 ? c->box.w : 100;
            int bh = c->box.h > 0 ? c->box.h : TYPE_BODY + 16;
            render_button(c, draw_x, draw_y, bw, bh);
            continue;
        }

        /* <textarea> — render multi-line text box */
        if (str_eq(c->tag, "textarea")) {
            int tw = c->box.w > 0 ? c->box.w : 300;
            int th = c->box.h > 0 ? c->box.h : TYPE_BODY * 6;
            render_textarea(c, draw_x, draw_y, tw, th);
            continue;
        }

        /* Text — render with TTF font system, respecting text-align */
        if (c->type == DOM_TEXT && c->text[0]) {
            font_id_t fid = text_font(c);
            int text_x = draw_x;

            /* text-align: check parent's alignment */
            int align = 0;  /* default left */
            if (c->parent) align = c->parent->style.text_align;

            if (align == 1) {
                /* center */
                int tw = font_measure(c->text, fid, c->style.font_size);
                int avail = c->box.w > 0 ? c->box.w : viewport_w;
                text_x = draw_x + (avail - tw) / 2;
            } else if (align == 2) {
                /* right */
                int tw = font_measure(c->text, fid, c->style.font_size);
                int avail = c->box.w > 0 ? c->box.w : viewport_w;
                text_x = draw_x + avail - tw;
            }

            font_draw(text_x, draw_y, c->text, fid,
                      c->style.font_size, c->style.color);
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
    browser_ensure_undo(b);
    dirty_clear(BROWSER_WIN_ID);
}

void browser_url_type(browser_t *b, char ch) {
    if (!b || ch == 0) return;
    browser_ensure_undo(b);
    int n = 0; while (b->url[n]) n++;
    if (n >= (int)sizeof(b->url) - 1) return;
    char ins[2] = { ch, 0 };
    undo_record(&s_url_undo, n, "", ins);
    b->url[n] = ch; b->url[n + 1] = 0;
    dirty_register(BROWSER_WIN_ID);
}

void browser_url_backspace(browser_t *b) {
    if (!b) return;
    browser_ensure_undo(b);
    int n = 0; while (b->url[n]) n++;
    if (n == 0) return;
    char del[2] = { b->url[n - 1], 0 };
    undo_record(&s_url_undo, n - 1, del, "");
    b->url[n - 1] = 0;
    dirty_register(BROWSER_WIN_ID);
}

void browser_history_render_state(browser_t *b, int x, int y, int w, int h) {
    list_state_t st = LIST_OK;
    const char *msg = 0;
    if (b->history_len == 0) { st = LIST_EMPTY; msg = "No history yet."; }
    else if (b->http_status < 0) { st = LIST_ERROR; msg = "Couldn't connect."; }
    if (st == LIST_OK) return;
    list_state_ctx_t ctx = { x, y, w, h, st, msg, 0, 0, 0 };
    list_render_state(&ctx);
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

/* Forward declarations for image fetch helpers used during navigation */
static void resolve_url(browser_t *b, const char *href, char *out, int max);
static dom_node_t *hit_test_link(dom_node_t *node, int px, int py,
                                  int parent_x, int parent_y);

/* Fetch URL into a kmalloc'd buffer. Caller must kfree(*out_buf).
 * Sets *out_len. Returns HTTP status (>=0) or -1 on failure.
 * Supports http:// and https://. */
#define IMG_FETCH_MAX 524288  /* 512 KB cap per image */

static int fetch_url(const char *url, uint8_t **out_buf, int *out_len)
{
    *out_buf = 0;
    *out_len = 0;

    char hostname[256], path[1024];
    int use_tls = 0;
    parse_url(url, hostname, 256, path, 1024, &use_tls);
    if (!hostname[0]) return -1;

    char *buf = (char *)kmalloc(IMG_FETCH_MAX);
    if (!buf) return -1;

    int status = -1;
    int body_len = 0;

    if (use_tls) {
        status = https_get(hostname, path, buf, IMG_FETCH_MAX, &body_len);
    } else {
        struct http_response resp;
        status = http_get(hostname, path, &resp);
        if (status >= 0) {
            body_len = resp.body_len;
            if (body_len > IMG_FETCH_MAX) body_len = IMG_FETCH_MAX;
            for (int i = 0; i < body_len; i++) buf[i] = resp.body[i];
        }
    }

    if (status < 0 || body_len <= 0) {
        kfree(buf);
        return -1;
    }

    *out_buf = (uint8_t *)buf;
    *out_len = body_len;
    return status;
}

/* Walk the DOM and, for every <img src="...">, fetch + decode the PNG.
 * Caches RGBA pixels in node->img_pixels and dimensions in img_w/img_h.
 * On any failure the node remains un-decoded and renders as a placeholder. */
static void decode_images(browser_t *b, dom_node_t *node)
{
    if (!node) return;

    if (node->type == DOM_ELEMENT && str_eq(node->tag, "img") &&
        node->attr_src[0] && !node->img_pixels)
    {
        char abs_url[2048];
        resolve_url(b, node->attr_src, abs_url, sizeof(abs_url));

        uint8_t *body = 0;
        int body_len = 0;
        int status = fetch_url(abs_url, &body, &body_len);
        if (status >= 200 && status < 300 && body && body_len > 0) {
            unsigned char *pixels = 0;
            unsigned w = 0, h = 0;
            unsigned err = lodepng_decode32(&pixels, &w, &h,
                                            body, (size_t)body_len);
            if (!err && pixels && w > 0 && h > 0 &&
                w <= 4096 && h <= 4096) {
                node->img_pixels = pixels;
                node->img_w = (int)w;
                node->img_h = (int)h;
            } else if (pixels) {
                lodepng_free(pixels);
            }
        }
        if (body) kfree(body);
    }

    for (dom_node_t *c = node->first_child; c; c = c->next_sibling)
        decode_images(b, c);
}

/* (Re)compute block layout + total page height for the current DOM against
 * the current surface width. Split out so draw_content can re-lay-out when the
 * WM finally sizes the surface (surface_w is 0 during the initial navigate). */
void browser_layout(browser_t *b)
{
    if (!b->dom) return;
    int content_w = b->surface_w - 16;   /* 8px padding each side */
    layout_compute(b->dom, content_w, b->surface_h - 96);  /* toolbar + status */
    b->page_height = 0;
    for (dom_node_t *c = b->dom->first_child; c; c = c->next_sibling) {
        int bottom = c->box.y + c->box.h + c->style.margin[2];
        if (bottom > b->page_height) b->page_height = bottom;
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

    /* Fetch page — 256 KB allows medium articles (Wikipedia ~80 KB,
     * news sites ~200 KB) without silent truncation. */
    static char page_buf[262144];
    int status = -1;

    /* Built-in offline pages (test:NAME) — drive the full
     * parse/style/layout/render path with no network so link hit-test,
     * forms and the scrollbar can be verified deterministically. */
    int builtin = (url[0]=='t'&&url[1]=='e'&&url[2]=='s'&&url[3]=='t'&&url[4]==':');
    if (builtin) {
        const char *html;
        if (str_eq(url, "test:dom2")) {
            /* I.7: querySelector / querySelectorAll / innerHTML / className. */
            html = "<html><head><title>DOM2</title></head><body>"
                   "<h1>DOM2 Demo</h1>"
                   "<div id=\"app\">placeholder</div>"
                   "<ul><li class=\"item\">a</li><li class=\"item\">b</li><li class=\"item\">c</li></ul>"
                   "<script>"
                   "var n = document.querySelectorAll('.item').length;"
                   "document.getElementById('app').innerHTML = '<h2 class=\"hdr\">Built via innerHTML</h2><p>found ' + n + ' items</p>';"
                   "var h = document.querySelector('.hdr'); h.className = 'hdr done';"
                   "console.log('qsa items', n, 'hdr class', document.querySelector('.hdr').className);"
                   "</script>"
                   "</body></html>";
        } else if (str_eq(url, "test:dom3")) {
            /* I.7: classList / remove() / .value / .children / .parentNode. */
            html = "<html><head><title>DOM3</title></head><body>"
                   "<h1 id=\"h\">DOM3 Demo</h1>"
                   "<div id=\"box\" class=\"card\">"
                     "<p class=\"one\">alpha</p><p class=\"two gone\">beta</p><p>gamma</p>"
                   "</div>"
                   "<input id=\"fld\" value=\"typed\">"
                   "<p id=\"out\">out</p>"
                   "<script>"
                   "var box = document.getElementById('box');"
                   "box.classList.add('active'); box.classList.toggle('card');"   /* card off, active on */
                   "document.querySelector('.gone').remove();"                     /* drop 'beta' */
                   "var kids = box.children.length;"
                   "var val = document.getElementById('fld').value;"
                   "var par = document.querySelector('.one').parentNode.id;"
                   "var has = box.classList.contains('active');"
                   "var first = box.firstElementChild.textContent;"           /* alpha */
                   "var last = box.lastElementChild.textContent;"             /* gamma */
                   "var nxt = box.firstElementChild.nextElementSibling.textContent;" /* gamma (beta gone) */
                   "document.getElementById('out').textContent = "
                     "'class=' + box.className + ' kids=' + kids + ' val=' + val + ' parent=' + par + ' active=' + has "
                     "+ ' first=' + first + ' last=' + last + ' next=' + nxt;"
                   "console.log('dom3', box.className, 'kids', kids, 'val', val, 'parent', par, 'active', has, 'first', first, 'last', last, 'next', nxt);"
                   "</script>"
                   "</body></html>";
        } else if (str_eq(url, "test:flex")) {
            /* I.7: CSS flexbox — row with justify-content, gap; column; align. */
            html = "<html><head><title>Flex</title></head><body>"
                   "<h1>Flex Demo</h1>"
                   "<div id=\"row\" style=\"display:flex; gap:10px; justify-content:space-between;\">"
                     "<div id=\"a\" style=\"background:#204060; padding:8px;\">A</div>"
                     "<div id=\"b\" style=\"background:#206040; padding:8px;\">B</div>"
                     "<div id=\"c\" style=\"background:#604020; padding:8px;\">C</div>"
                   "</div>"
                   "<div id=\"col\" style=\"display:flex; flex-direction:column; gap:6px;\">"
                     "<div id=\"x\" style=\"background:#402060; padding:6px;\">X</div>"
                     "<div id=\"y\" style=\"background:#602040; padding:6px;\">Y</div>"
                   "</div>"
                   "<p id=\"out\">out</p>"
                   "<script>"
                   "function L(id){return document.getElementById(id).offsetLeft;}"
                   "function T(id){return document.getElementById(id).offsetTop;}"
                   "var rowSame = (T('a')==T('b') && T('b')==T('c'));"          /* row: same top */
                   "var ordered = (L('a') < L('b') && L('b') < L('c'));"        /* left to right */
                   "var spread  = (L('c') - L('a'));"                           /* space-between spreads them */
                   "var colStacked = (T('y') > T('x') && L('x')==L('y'));"       /* col: stacked, same left */
                   "document.getElementById('out').textContent = "
                     "'rowSame=' + rowSame + ' ordered=' + ordered + ' spread=' + spread + ' colStacked=' + colStacked;"
                   "console.log('flex rowSame', rowSame, 'ordered', ordered, 'spread', spread, "
                     "'La', L('a'), 'Lb', L('b'), 'Lc', L('c'), 'colStacked', colStacked, 'Tx', T('x'), 'Ty', T('y'));"
                   "</script>"
                   "</body></html>";
        } else if (str_eq(url, "test:grid")) {
            /* I.7: CSS grid — 3 columns, 6 items => 2 rows, row-major flow. */
            html = "<html><head><title>Grid</title></head><body>"
                   "<h1>Grid Demo</h1>"
                   "<div id=\"g\" style=\"display:grid; grid-template-columns:repeat(3, 1fr); gap:8px;\">"
                     "<div id=\"i0\" style=\"background:#204060; padding:8px;\">0</div>"
                     "<div id=\"i1\" style=\"background:#206040; padding:8px;\">1</div>"
                     "<div id=\"i2\" style=\"background:#604020; padding:8px;\">2</div>"
                     "<div id=\"i3\" style=\"background:#402060; padding:8px;\">3</div>"
                     "<div id=\"i4\" style=\"background:#602040; padding:8px;\">4</div>"
                     "<div id=\"i5\" style=\"background:#206060; padding:8px;\">5</div>"
                   "</div>"
                   "<p id=\"out\">out</p>"
                   "<script>"
                   "function L(id){return document.getElementById(id).offsetLeft;}"
                   "function T(id){return document.getElementById(id).offsetTop;}"
                   "var row0 = (T('i0')==T('i1') && T('i1')==T('i2'));"          /* first row aligned */
                   "var row1 = (T('i3')==T('i4') && T('i4')==T('i5'));"          /* second row aligned */
                   "var wrapped = (T('i3') > T('i0'));"                          /* item 3 wrapped below */
                   "var cols = (L('i0') < L('i1') && L('i1') < L('i2'));"        /* columns left to right */
                   "var aligned = (L('i0')==L('i3') && L('i2')==L('i5'));"       /* same column same x */
                   "document.getElementById('out').textContent = "
                     "'row0=' + row0 + ' row1=' + row1 + ' wrapped=' + wrapped + ' cols=' + cols + ' aligned=' + aligned;"
                   "console.log('grid row0', row0, 'row1', row1, 'wrapped', wrapped, 'cols', cols, 'aligned', aligned);"
                   "</script>"
                   "</body></html>";
        } else if (str_eq(url, "test:fetch")) {
            /* I.7 fetch(): a script pulls a real URL over the net stack and
             * writes the result into the DOM from a Promise .then chain. */
            html = "<html><head><title>Fetch</title></head><body>"
                   "<h1>Fetch Demo</h1>"
                   "<p id=\"out\">loading...</p>"
                   "<script>"
                   "fetch('http://example.com/').then(function(r){"
                   "  console.log('fetch status', r.status, 'ok', r.ok);"
                   "  return r.text();"
                   "}).then(function(t){"
                   "  document.getElementById('out').textContent = 'fetched ' + t.length + ' bytes over the net';"
                   "  console.log('fetch body bytes', t.length);"
                   "});"
                   "</script>"
                   "</body></html>";
        } else if (str_eq(url, "test:build")) {
            /* I.7 createElement/appendChild: a script builds a list from scratch. */
            html = "<html><head><title>Build</title></head><body>"
                   "<h1>Build Demo</h1>"
                   "<ul id=\"list\"></ul>"
                   "<script>"
                   "var list = document.getElementById('list');"
                   "for (var i = 1; i <= 3; i++) {"
                   "  var li = document.createElement('li');"
                   "  li.textContent = 'Item number ' + i + ' (' + (i*i) + ')';"
                   "  list.appendChild(li);"
                   "}"
                   "console.log('built', list.tagName, 'with', 3, 'items');"
                   "</script>"
                   "</body></html>";
        } else if (str_eq(url, "test:events")) {
            /* I.7 events: a button whose click handler mutates the DOM. */
            html = "<html><head><title>Events</title></head><body>"
                   "<h1 id=\"h\">Counter Demo</h1>"
                   "<p id=\"count\">clicks: 0</p>"
                   "<button id=\"btn\">Click me</button>"
                   "<script>"
                   "var n = 0;"
                   "document.getElementById('btn').addEventListener('click', function(e){"
                   "  n = n + 1;"
                   "  document.getElementById('count').textContent = 'clicks: ' + n;"
                   "  console.log('clicked; n =', n, 'target =', e.target.tagName);"
                   "});"
                   "</script>"
                   "</body></html>";
        } else if (str_eq(url, "test:js")) {
            /* I.7 proof: a <script> reads + rewrites the DOM; the page must
             * render the JS-set text, not the original. */
            html = "<html><head><title>JS Test</title></head><body>"
                   "<h1 id=\"title\">original heading</h1>"
                   "<p id=\"msg\">before script</p>"
                   "<script>"
                   "document.getElementById('title').textContent = 'Set by JavaScript';"
                   "document.getElementById('msg').textContent = '2 + 2 = ' + (2+2) + ', sqrt4 = ' + Math.sqrt(4);"
                   "console.log('script ran; title id =', document.getElementById('title').id);"
                   "</script>"
                   "</body></html>";
        } else if (str_eq(url, "test:page2")) {
            html = "<html><head><title>Page Two</title></head><body>"
                   "<h1>Page Two</h1>"
                   "<p>You followed the link — this is the second built-in page.</p>"
                   "<p><a href=\"test:home\">Back to the home page</a></p>"
                   "</body></html>";
        } else {
            html = "<html><head><title>Zeos Test</title></head><body>"
                   "<h1>Zeos Browser Test</h1>"
                   "<p>Built-in offline page to verify render, links, forms and scrolling.</p>"
                   "<p><a href=\"test:page2\">Follow this link</a> to load page two.</p>"
                   "<form><input type=\"text\"><button>Submit</button></form>"
                   "<hr>"
                   "<p>Filler 01 - the quick brown fox jumps over the lazy dog.</p>"
                   "<p>Filler 02 - the quick brown fox jumps over the lazy dog.</p>"
                   "<p>Filler 03 - the quick brown fox jumps over the lazy dog.</p>"
                   "<p>Filler 04 - the quick brown fox jumps over the lazy dog.</p>"
                   "<p>Filler 05 - the quick brown fox jumps over the lazy dog.</p>"
                   "<p>Filler 06 - the quick brown fox jumps over the lazy dog.</p>"
                   "<p>Filler 07 - the quick brown fox jumps over the lazy dog.</p>"
                   "<p>Filler 08 - the quick brown fox jumps over the lazy dog.</p>"
                   "<p>Filler 09 - the quick brown fox jumps over the lazy dog.</p>"
                   "<p>Filler 10 - the quick brown fox jumps over the lazy dog.</p>"
                   "<p>Filler 11 - the quick brown fox jumps over the lazy dog.</p>"
                   "<p>Filler 12 - the quick brown fox jumps over the lazy dog.</p>"
                   "<p>Filler 13 - the quick brown fox jumps over the lazy dog.</p>"
                   "<p>Filler 14 - the quick brown fox jumps over the lazy dog.</p>"
                   "<p>Filler 15 - the quick brown fox jumps over the lazy dog.</p>"
                   "<p>Filler 16 - the quick brown fox jumps over the lazy dog.</p>"
                   "<p>Filler 17 - the quick brown fox jumps over the lazy dog.</p>"
                   "<p>Filler 18 - the quick brown fox jumps over the lazy dog.</p>"
                   "<p>Filler 19 - the quick brown fox jumps over the lazy dog.</p>"
                   "<p>Filler 20 - end of the built-in test page.</p>"
                   "</body></html>";
        }
        int L = 0;
        while (html[L] && L < (int)sizeof(page_buf) - 1) { page_buf[L] = html[L]; L++; }
        page_buf[L] = 0;
        b->page_len = L;
        status = 0;
    } else if (use_tls) {
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

    /* Decode HTML entities in text nodes (in-place; output ≤ input) */
    decode_entities(b->dom);

    /* Apply styles */
    css_apply_defaults(b->dom);
    css_apply_inline(b->dom);

    /* Fetch + decode all <img> tags BEFORE layout so the layout pass
     * picks up real image dimensions instead of the 200x80 default. */
    decode_images(b, b->dom);

    /* Compute layout against the current surface width. NOTE: at open time
     * the WM hasn't sized the surface yet (surface_w == 0), so this first pass
     * lays out at width 0 — browser_app_draw_content re-runs browser_layout()
     * once the real width arrives, which is when links get correct box widths. */
    browser_layout(b);

    /* I.7: run the page's <script> tags now that the DOM + layout exist. If a
     * script mutated the DOM (textContent/setAttribute), re-lay-out so the
     * change is visible. */
    {
        extern void browser_run_scripts(dom_node_t *root);
        extern int  dom_take_dirty(void);
        browser_run_scripts(b->dom);
        if (dom_take_dirty()) {
            css_apply_defaults(b->dom);   /* style JS-created nodes */
            css_apply_inline(b->dom);
            browser_layout(b);
        }
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

    dirty_clear(BROWSER_WIN_ID);
    return 0;
}

/* ── Right-click context menu actions ── */
static void rc_action_open(void *ctx) {
    (void)ctx;
    if (!s_rc_browser || !s_rc_href[0]) return;
    char resolved[2048];
    resolve_url(s_rc_browser, s_rc_href, resolved, 2048);
    browser_navigate(s_rc_browser, resolved);
}
static void rc_action_open_new_tab(void *ctx) { rc_action_open(ctx); }
static void rc_action_copy_link(void *ctx) {
    (void)ctx;
    kputs("BROWSE: copy link: "); kputs(s_rc_href); kputs("\n");
}
static void rc_action_inspect(void *ctx) {
    (void)ctx;
    kputs("BROWSE: inspect link: "); kputs(s_rc_href); kputs("\n");
}

int browser_right_click(browser_t *b, int x, int y) {
    if (!b || !b->dom) return 0;
    int content_x = b->surface_x + 8;
    int content_y = b->surface_y + 48;
    int px = x - content_x;
    int py = y - content_y + b->scroll_y;
    dom_node_t *link = hit_test_link(b->dom, px, py, 0, 0);
    if (!link || !link->attr_href[0]) return 0;
    s_rc_browser = b;
    int hi = 0;
    while (link->attr_href[hi] && hi < 2047) { s_rc_href[hi] = link->attr_href[hi]; hi++; }
    s_rc_href[hi] = 0;
    static const ctx_menu_item_t items[4] = {
        { "Open",            rc_action_open,         0, 1 },
        { "Open in new tab", rc_action_open_new_tab, 0, 1 },
        { "Copy link",       rc_action_copy_link,    0, 1 },
        { "Inspect",         rc_action_inspect,      0, 1 },
    };
    context_menu_open(x, y, items, 4);
    return 1;
}

static void browser_register_link_hovers_walk(dom_node_t *node, int parent_x, int parent_y,
                                              int viewport_x, int viewport_y, int scroll_y)
{
    if (!node) return;
    for (dom_node_t *c = node->first_child; c; c = c->next_sibling) {
        if (c->style.display == 2) continue;
        int bx = parent_x + c->box.x;
        int by = parent_y + c->box.y;
        if (str_eq(c->tag, "a") && c->attr_href[0] && c->box.w > 0 && c->box.h > 0 &&
            s_link_token_count < BROWSER_MAX_LINK_HOVERS)
        {
            int sx = viewport_x + bx;
            int sy = viewport_y + by - scroll_y;
            uint64_t tok = hover_register(sx, sy, c->box.w, c->box.h,
                                          HOVER_CURSOR_POINTER, 0, 0);
            if (tok) s_link_tokens[s_link_token_count++] = tok;
        }
        int nx = parent_x + c->box.x + c->style.padding[3];
        int ny = parent_y + c->box.y + c->style.padding[0];
        browser_register_link_hovers_walk(c, nx, ny, viewport_x, viewport_y, scroll_y);
    }
}

static void browser_refresh_link_hovers(browser_t *b) {
    for (int i = 0; i < s_link_token_count; i++) hover_unregister(s_link_tokens[i]);
    s_link_token_count = 0;
    if (!b->dom) return;
    int cx = b->surface_x + 8;
    int cy = b->surface_y + 48;
    browser_register_link_hovers_walk(b->dom, 0, 0, cx, cy, b->scroll_y);
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
    browser_navigate(b, "https://example.com");
}

/* Scroll step for arrow keys and mouse wheel */
#define SCROLL_STEP 32

void browser_scroll(browser_t *b, int delta_y) {
    b->scroll_y += delta_y;
    if (b->scroll_y < 0) b->scroll_y = 0;
    int max_scroll = b->page_height - (b->surface_h - 96);
    if (max_scroll < 0) max_scroll = 0;
    if (b->scroll_y > max_scroll) b->scroll_y = max_scroll;
}

/* Handle keyboard input for the browser.
 * scancode: raw scancode from keyboard driver.
 * Up arrow = scroll up, Down arrow = scroll down,
 * Page Up/Down = scroll by viewport height. */
void browser_key(browser_t *b, uint8_t scancode) {
    switch (scancode) {
    case 0x48: browser_scroll(b, -SCROLL_STEP); break;  /* Up arrow */
    case 0x50: browser_scroll(b, SCROLL_STEP);  break;  /* Down arrow */
    case 0x49:  /* Page Up */
        browser_scroll(b, -(b->surface_h - 96));
        break;
    case 0x51:  /* Page Down */
        browser_scroll(b, b->surface_h - 96);
        break;
    case 0x47:  /* Home */
        b->scroll_y = 0;
        break;
    case 0x4F:  /* End */
        browser_scroll(b, b->page_height);
        break;
    }
}

/* Handle mouse wheel scroll. delta > 0 = scroll down, < 0 = scroll up.
 * Typical: each wheel notch = 3 lines worth of scroll. */
void browser_mouse_wheel(browser_t *b, int delta) {
    browser_scroll(b, delta * SCROLL_STEP * 3);
}

/* ── Link hit-testing ── */

/* Recursively find a link node at (px, py) in page coordinates */
static dom_node_t *hit_test_link(dom_node_t *node, int px, int py,
                                  int parent_x, int parent_y)
{
    if (!node) return 0;

    for (dom_node_t *c = node->first_child; c; c = c->next_sibling) {
        if (c->style.display == 2) continue;

        int nx = parent_x + c->box.x + c->style.padding[3];
        int ny = parent_y + c->box.y + c->style.padding[0];

        /* Check if point is inside this node's box */
        int bx = parent_x + c->box.x;
        int by = parent_y + c->box.y;
        if (px >= bx && px < bx + c->box.w &&
            py >= by && py < by + c->box.h) {
            /* If this is an <a> tag with href, we found it */
            if (str_eq(c->tag, "a") && c->attr_href[0])
                return c;
        }

        /* Recurse into children */
        dom_node_t *found = hit_test_link(c, px, py, nx, ny);
        if (found) return found;
    }
    return 0;
}

/* Resolve a possibly-relative URL against the current page */
static void resolve_url(browser_t *b, const char *href, char *out, int max) {
    if (str_starts(href, "http://") || str_starts(href, "https://") ||
        str_starts(href, "test:")) {
        /* Absolute URL (or a built-in test: page) — use as-is */
        int i = 0;
        while (href[i] && i < max - 1) { out[i] = href[i]; i++; }
        out[i] = 0;
        return;
    }

    /* Build from current origin */
    char *p = out;
    char *end = out + max - 1;

    /* Determine scheme */
    const char *scheme = "http://";
    if (str_starts(b->url, "https://")) scheme = "https://";

    while (*scheme && p < end) *p++ = *scheme++;
    const char *h = b->hostname;
    while (*h && p < end) *p++ = *h++;

    if (href[0] == '/') {
        /* Absolute path */
        while (*href && p < end) *p++ = *href++;
    } else if (href[0] == '#') {
        /* Anchor on current page */
        const char *pa = b->path;
        while (*pa && p < end) *p++ = *pa++;
        while (*href && p < end) *p++ = *href++;
    } else {
        /* Relative path — append to current directory */
        const char *pa = b->path;
        /* Find last '/' in path */
        int last_slash = 0;
        for (int i = 0; pa[i]; i++) {
            if (pa[i] == '/') last_slash = i;
        }
        for (int i = 0; i <= last_slash && p < end; i++) *p++ = pa[i];
        while (*href && p < end) *p++ = *href++;
    }
    *p = 0;
}

/* Deepest ELEMENT whose box contains the point (for JS click dispatch). */
static dom_node_t *hit_test_element(dom_node_t *node, int px, int py,
                                    int parent_x, int parent_y)
{
    if (!node) return 0;
    for (dom_node_t *c = node->first_child; c; c = c->next_sibling) {
        if (c->style.display == 2) continue;
        int bx = parent_x + c->box.x;
        int by = parent_y + c->box.y;
        int nx = bx + c->style.padding[3];
        int ny = by + c->style.padding[0];
        /* deepest first */
        dom_node_t *deeper = hit_test_element(c, px, py, nx, ny);
        if (deeper) return deeper;
        if (c->type == DOM_ELEMENT && c->box.w > 0 && c->box.h > 0 &&
            px >= bx && px < bx + c->box.w && py >= by && py < by + c->box.h)
            return c;
    }
    return 0;
}

void browser_click(browser_t *b, int x, int y) {
    if (!b->dom) return;

    /* Convert viewport click to page coordinates */
    int content_x = b->surface_x + 8;
    int content_y = b->surface_y + 48;
    int px = x - content_x;
    int py = y - content_y + b->scroll_y;

    /* Hit-test for links */
    dom_node_t *link = hit_test_link(b->dom, px, py, 0, 0);
    if (link && link->attr_href[0]) {
        /* Handle anchor links: scroll to element (simplified) */
        if (link->attr_href[0] == '#') {
            /* For now, just scroll to top on anchor click */
            b->scroll_y = 0;
            return;
        }

        /* Resolve and navigate */
        char resolved[2048];
        resolve_url(b, link->attr_href, resolved, 2048);
        browser_navigate(b, resolved);
        return;
    }

    /* JS click events: dispatch to the element under the pointer, bubbling up
     * the ancestor chain. If a handler mutated the DOM, re-lay-out. */
    dom_node_t *el = hit_test_element(b->dom, px, py, 0, 0);
    if (el) {
        extern int zeos_js_dispatch_event(dom_node_t *node, const char *type);
        extern int dom_take_dirty(void);
        int fired = 0;
        for (dom_node_t *t = el; t; t = t->parent)
            fired += zeos_js_dispatch_event(t, "click");
        if (fired && dom_take_dirty()) {
            css_apply_defaults(b->dom);   /* style JS-created nodes */
            css_apply_inline(b->dom);
            browser_layout(b);
        }
    }
}

/* ── Scrollbar constants ── */
#define SCROLLBAR_WIDTH   8
#define SCROLLBAR_MIN_THUMB 24

/* Scrollbar state for drag tracking */
static int scrollbar_dragging = 0;
static int scrollbar_drag_offset = 0;  /* offset from top of thumb to click point */

void browser_draw(browser_t *b) {
    if (!b->dom) return;

    /* Clear content area */
    int cx = b->surface_x + 8;
    int cy = b->surface_y + 48;  /* Below toolbar */
    int cw = b->surface_w - 16;
    int ch = b->surface_h - 96;  /* Toolbar + status */

    fb_rect(cx, cy, cw, ch, COLOR_SURFACE);

    /* Render page (narrowed to leave room for scrollbar). Clip to the content
     * viewport (intersected with any existing compositor clip) so nested blocks
     * — which clip only against their own box height in the recursion — cannot
     * bleed past the window bottom onto the desktop. */
    int page_w = cw - SCROLLBAR_WIDTH - 2;  /* 2px gap before scrollbar */
    int sx0, sy0, sx1, sy1;
    fb_get_clip(&sx0, &sy0, &sx1, &sy1);
    int rx0 = cx, ry0 = cy, rx1 = cx + cw, ry1 = cy + ch;
    if (rx0 < sx0) rx0 = sx0;
    if (ry0 < sy0) ry0 = sy0;
    if (rx1 > sx1) rx1 = sx1;
    if (ry1 > sy1) ry1 = sy1;
    if (rx1 > rx0 && ry1 > ry0)
        fb_set_clip(rx0, ry0, rx1 - rx0, ry1 - ry0);
    render_page(b->dom, cx, cy, b->scroll_y, page_w, ch);
    fb_set_clip(sx0, sy0, sx1 - sx0, sy1 - sy0);  /* restore prior clip */

    /* Refresh link hover zones — cursor changes to hand over <a>. */
    browser_refresh_link_hovers(b);

    /* ── Scrollbar ── */
    if (b->page_height > ch) {
        int track_x = cx + cw - SCROLLBAR_WIDTH;
        int track_y = cy;
        int track_h = ch;

        /* Track */
        fb_rect(track_x, track_y, SCROLLBAR_WIDTH, track_h, COLOR_SURFACE_TOP);

        /* Thumb size: proportional to viewport / page ratio */
        int thumb_h = (ch * track_h) / b->page_height;
        if (thumb_h < SCROLLBAR_MIN_THUMB) thumb_h = SCROLLBAR_MIN_THUMB;

        /* Thumb position */
        int max_scroll = b->page_height - ch;
        if (max_scroll < 1) max_scroll = 1;
        int thumb_y = track_y + (b->scroll_y * (track_h - thumb_h)) / max_scroll;

        /* Hover detection — check if mouse is over the thumb */
        int mx = mouse_get_x();
        int my = mouse_get_y();
        int thumb_hover = (mx >= track_x && mx < track_x + SCROLLBAR_WIDTH &&
                           my >= thumb_y && my < thumb_y + thumb_h);

        uint32_t thumb_color = thumb_hover ? COLOR_ON_SURFACE_3 : COLOR_ON_SURFACE_4;
        fb_rect(track_x, thumb_y, SCROLLBAR_WIDTH, thumb_h, thumb_color);
    }
}

/* Handle scrollbar click/drag. Returns 1 if the click was on the scrollbar. */
int browser_scrollbar_click(browser_t *b, int x, int y) {
    int cx = b->surface_x + 8;
    int cy = b->surface_y + 48;
    int cw = b->surface_w - 16;
    int ch = b->surface_h - 96;

    if (b->page_height <= ch) return 0;  /* No scrollbar visible */

    int track_x = cx + cw - SCROLLBAR_WIDTH;
    int track_y = cy;
    int track_h = ch;

    /* Check if click is on the scrollbar track */
    if (x < track_x || x >= track_x + SCROLLBAR_WIDTH) return 0;
    if (y < track_y || y >= track_y + track_h) return 0;

    /* Compute thumb geometry */
    int thumb_h = (ch * track_h) / b->page_height;
    if (thumb_h < SCROLLBAR_MIN_THUMB) thumb_h = SCROLLBAR_MIN_THUMB;

    int max_scroll = b->page_height - ch;
    if (max_scroll < 1) max_scroll = 1;
    int thumb_y = track_y + (b->scroll_y * (track_h - thumb_h)) / max_scroll;

    if (y >= thumb_y && y < thumb_y + thumb_h) {
        /* Click on thumb — start drag */
        scrollbar_dragging = 1;
        scrollbar_drag_offset = y - thumb_y;
    } else if (y < thumb_y) {
        /* Click above thumb — page up */
        browser_scroll(b, -ch);
    } else {
        /* Click below thumb — page down */
        browser_scroll(b, ch);
    }
    return 1;
}

/* Handle scrollbar drag (call each frame while mouse is held) */
void browser_scrollbar_drag(browser_t *b, int y) {
    if (!scrollbar_dragging) return;

    int cy = b->surface_y + 48;
    int ch = b->surface_h - 96;
    int track_y = cy;
    int track_h = ch;

    int thumb_h = (ch * track_h) / b->page_height;
    if (thumb_h < SCROLLBAR_MIN_THUMB) thumb_h = SCROLLBAR_MIN_THUMB;

    int max_scroll = b->page_height - ch;
    if (max_scroll < 1) max_scroll = 1;

    /* Map mouse Y to scroll position */
    int thumb_top = y - scrollbar_drag_offset;
    int scroll_range = track_h - thumb_h;
    if (scroll_range < 1) scroll_range = 1;

    int new_scroll = ((thumb_top - track_y) * max_scroll) / scroll_range;
    if (new_scroll < 0) new_scroll = 0;
    if (new_scroll > max_scroll) new_scroll = max_scroll;
    b->scroll_y = new_scroll;
}

/* Release scrollbar drag */
void browser_scrollbar_release(void) {
    scrollbar_dragging = 0;
}

void browser_draw_toolbar(browser_t *b) {
    int tx = b->surface_x;
    int ty = b->surface_y;
    int tw = b->surface_w;

    /* Toolbar background */
    fb_rect(tx, ty, tw, 48, COLOR_SURFACE_HIGH);

    /* Navigation buttons (text placeholders until icon rendering works) */
    font_draw(tx + 8, ty + 14, "<", FONT_UI, TYPE_LABEL, COLOR_ON_SURFACE);
    font_draw(tx + 24, ty + 14, ">", FONT_UI, TYPE_LABEL, COLOR_ON_SURFACE);
    font_draw(tx + 40, ty + 14, "R", FONT_UI, TYPE_LABEL, COLOR_ON_SURFACE);

    /* URL bar */
    fb_rect(tx + 64, ty + 8, tw - 128, 32, COLOR_SURFACE_TOP);
    font_draw(tx + 72, ty + 14, b->url, FONT_UI, TYPE_LABEL, COLOR_ON_SURFACE_2);

    /* Separator */
    fb_hline(tx, ty + 47, tw, COLOR_SEPARATOR);

    /* ── Hover zones for nav buttons + URL bar ── */
    int back_x = tx + 4,  back_y = ty + 8, back_w = 16, back_h = 32;
    int fwd_x  = tx + 20, fwd_y  = ty + 8, fwd_w  = 16, fwd_h  = 32;
    int rfr_x  = tx + 36, rfr_y  = ty + 8, rfr_w  = 16, rfr_h  = 32;
    int home_x = tx + 52, home_y = ty + 8, home_w = 12, home_h = 32;
    int url_x  = tx + 64, url_y  = ty + 8;
    int url_w  = tw - 128, url_h = 32;
    if (!s_hov_inited) {
        s_hov_back    = hover_register(back_x, back_y, back_w, back_h, HOVER_CURSOR_POINTER, 0, 0);
        s_hov_fwd     = hover_register(fwd_x,  fwd_y,  fwd_w,  fwd_h,  HOVER_CURSOR_POINTER, 0, 0);
        s_hov_refresh = hover_register(rfr_x,  rfr_y,  rfr_w,  rfr_h,  HOVER_CURSOR_POINTER, 0, 0);
        s_hov_home    = hover_register(home_x, home_y, home_w, home_h, HOVER_CURSOR_POINTER, 0, 0);
        s_hov_url     = hover_register(url_x,  url_y,  url_w,  url_h,  HOVER_CURSOR_TEXT,    0, 0);
        s_hov_inited = 1;
    } else {
        hover_update_rect(s_hov_back,    back_x, back_y, back_w, back_h);
        hover_update_rect(s_hov_fwd,     fwd_x,  fwd_y,  fwd_w,  fwd_h);
        hover_update_rect(s_hov_refresh, rfr_x,  rfr_y,  rfr_w,  rfr_h);
        hover_update_rect(s_hov_home,    home_x, home_y, home_w, home_h);
        hover_update_rect(s_hov_url,     url_x,  url_y,  url_w,  url_h);
    }
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

    font_draw(sx + 8, sy + 4, status, FONT_UI, TYPE_CAPTION, COLOR_ON_SURFACE_3);
}

/* ── Browser as a WM app ─────────────────────────────────────────────
 * The browser was dormant (parser/layout/render existed but nothing
 * opened it). Wire it into an openable desktop surface + a `browse <url>`
 * shell command so section I of the BUILD_MAP is reachable/verifiable.
 * The draw_content callback points the browser's viewport at the WM
 * content rect and renders into it each composite. */
extern int  wm_create_surface(const char *, int, int, int, int, int,
                              void (*)(int, int, int, int, int));
extern void wm_force_visible(int);
extern void wm_focus_surface(int);

static browser_t g_browser_app;
static int       g_browser_surface = -1;
static int       g_browser_active  = 0;

static int g_browser_laid_w = -1;
static int g_browser_laid_h = -1;

static void browser_app_draw_content(int id, int x, int y, int w, int h)
{
    (void)id;
    g_browser_app.surface_x = x;
    g_browser_app.surface_y = y;
    g_browser_app.surface_w = w;
    g_browser_app.surface_h = h;
    /* The surface is sized by the WM only after browser_navigate() ran (with
     * surface_w == 0), so re-lay-out the DOM the first time — and whenever the
     * window is resized — the real content width is known. Without this, every
     * box has width surface_w-16 == -16 and links become un-hittable. */
    if (g_browser_app.dom && (w != g_browser_laid_w || h != g_browser_laid_h)) {
        browser_layout(&g_browser_app);
        g_browser_laid_w = w;
        g_browser_laid_h = h;
    }
    browser_draw(&g_browser_app);
    browser_draw_toolbar(&g_browser_app);
    browser_draw_status(&g_browser_app);
}

int browser_app_active(void) { return g_browser_active; }

/* Route a screen-space click into the active browser app. This is the entry
 * point the compositor input path should call for the Browser surface; the
 * `bclick` diagnostic command uses it to verify link hit-test → navigate. */
void browser_app_click(int x, int y)
{
    if (!g_browser_active) return;
    char prev[2048];
    int i = 0; for (; g_browser_app.url[i] && i < 2047; i++) prev[i] = g_browser_app.url[i];
    prev[i] = 0;
    (void)prev;
    browser_click(&g_browser_app, x, y);
    /* Always recomposite after a click: it may have navigated (URL change) or
     * a JS click handler may have mutated + re-laid-out the current page. The
     * compositor only redraws dirty regions at idle, so force a full repaint. */
    { extern void compositor_dirty_all(void); compositor_dirty_all(); }
}

/* Open (or focus) the browser app and navigate to url. Returns
 * browser_navigate()'s result (0 = ok), or -1 if the surface won't create. */
int browser_app_open(const char *url)
{
    if (!g_browser_active) {
        browser_init(&g_browser_app);
        g_browser_surface = wm_create_surface("Browser", -1,
                                              120, 120, 1000, 680,
                                              browser_app_draw_content);
        if (g_browser_surface < 0) return -1;
        g_browser_active = 1;
    }
    int rc = 0;
    if (url && url[0]) rc = browser_navigate(&g_browser_app, url);
    wm_force_visible(g_browser_surface);
    wm_focus_surface(g_browser_surface);
    /* Force a full recomposite so the newly-created surface actually shows —
     * at idle the compositor only redraws on a dirty region. */
    { extern void compositor_dirty_all(void); compositor_dirty_all(); }
    return rc;
}
