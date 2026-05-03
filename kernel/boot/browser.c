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
    }
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
                    else if (str_eq(attr_name, "type"))
                        read_attr_val(&p, end, node->attr_type, 32);
                    else if (str_eq(attr_name, "value"))
                        read_attr_val(&p, end, node->attr_value, 256);
                    else if (str_eq(attr_name, "alt"))
                        read_attr_val(&p, end, node->attr_alt, 256);
                    else if (str_eq(attr_name, "placeholder"))
                        read_attr_val(&p, end, node->attr_placeholder, 256);
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

    /* display: none|block|inline */
    if (css_prop_eq(pbuf, "display")) {
        if (str_starts(v, "block"))  node->style.display = 0;
        if (str_starts(v, "inline")) node->style.display = 1;
        if (str_starts(v, "none"))   node->style.display = 2;
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
    uint32_t bg = 0xFF1A2633;  /* dark blue-tinted rect */
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

    /* Decode HTML entities in text nodes (in-place; output ≤ input) */
    decode_entities(b->dom);

    /* Apply styles */
    css_apply_defaults(b->dom);
    css_apply_inline(b->dom);

    /* Fetch + decode all <img> tags BEFORE layout so the layout pass
     * picks up real image dimensions instead of the 200x80 default. */
    decode_images(b, b->dom);

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
    if (str_starts(href, "http://") || str_starts(href, "https://")) {
        /* Absolute URL — use as-is */
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

    /* Render page (narrowed to leave room for scrollbar) */
    int page_w = cw - SCROLLBAR_WIDTH - 2;  /* 2px gap before scrollbar */
    render_page(b->dom, cx, cy, b->scroll_y, page_w, ch);

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
