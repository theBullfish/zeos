/*
 * Zeos — notes-zeos: real-time backlink + search index for .md files.
 *
 * Subscriber over fs_event. Every .md create / write / unlink / rename
 * re-resolves that file's contribution to the indices.
 *
 *   backlinks_idx : key=hash32(target_basename)   value=hash32(source_path)
 *   search_idx    : key=hash32(token)             value=doc_id
 *   docs_idx      : key=doc_id                    value=hash32(path)
 *
 * Storage: std.btree (in-memory, btree_insert / btree_range).
 * Wikilink extraction: hand-rolled state machine matching std.regex
 * pattern \[\[([^\]]+)\]\]. The .zp file (programs/notes-zeos.zp)
 * declares the chain shape.
 *
 * Codex Labs LLC — 2026
 */

#include "notes_zeos.h"
#include "std_btree.h"
#include "fs_event.h"
#include "fat32.h"
#include "vault.h"
#include "kprint.h"

/* ── State ─────────────────────────────────────────────────────── */

#define NOTES_MAX_DOCS       512
#define NOTES_READ_BUF       16384
#define NOTES_TOKEN_MAX      64

struct notes_doc {
    int      used;
    uint32_t path_hash;
    char     path[256];
    uint32_t token_count;
    uint32_t link_count;
};

static struct notes_doc g_docs[NOTES_MAX_DOCS];
static int  g_doc_count    = 0;
static int  g_initialized  = 0;
static int  g_listener_on  = 0;

static btree_handle_t g_backlinks = -1;
static btree_handle_t g_search    = -1;
static btree_handle_t g_docs_idx  = -1;

static uint32_t g_total_tokens    = 0;
static uint32_t g_total_backlinks = 0;

/* ── Tiny string helpers (avoid pulling string.h dependency) ───── */

static int nz_streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

static int nz_strlen(const char *s) {
    int n = 0; while (s && s[n]) n++; return n;
}

static void nz_strcpy(char *dst, const char *src, int max) {
    int i = 0;
    while (src && src[i] && i + 1 < max) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

/* FNV-1a 32-bit. Truncated to int31 for btree (which uses int32_t and
 * negatives are reserved). */
static int32_t nz_hash(const char *s, int len) {
    uint32_t h = 0x811c9dc5u;
    for (int i = 0; i < len; i++) {
        h ^= (uint8_t)s[i];
        h *= 0x01000193u;
    }
    return (int32_t)(h & 0x7fffffffu);
}

/* Lowercase a single ASCII byte. */
static char nz_tolower(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c;
}

/* Returns 1 if path ends in ".md" (case-insensitive). */
static int nz_is_md(const char *path) {
    int n = nz_strlen(path);
    if (n < 3) return 0;
    char a = nz_tolower(path[n - 3]);
    char b = nz_tolower(path[n - 2]);
    char c = nz_tolower(path[n - 1]);
    return a == '.' && b == 'm' && c == 'd';
}

/* Basename: pointer to last '/' + 1, or whole path if none. */
static const char *nz_basename(const char *p) {
    const char *b = p;
    for (const char *q = p; *q; q++) if (*q == '/') b = q + 1;
    return b;
}

/* ── Doc table ─────────────────────────────────────────────────── */

static int nz_doc_find(const char *path) {
    uint32_t h = (uint32_t)nz_hash(path, nz_strlen(path));
    for (int i = 0; i < NOTES_MAX_DOCS; i++) {
        if (g_docs[i].used && g_docs[i].path_hash == h
            && nz_streq(g_docs[i].path, path))
            return i;
    }
    return -1;
}

static int nz_doc_alloc(const char *path) {
    for (int i = 0; i < NOTES_MAX_DOCS; i++) {
        if (!g_docs[i].used) {
            g_docs[i].used = 1;
            g_docs[i].path_hash = (uint32_t)nz_hash(path, nz_strlen(path));
            nz_strcpy(g_docs[i].path, path, sizeof(g_docs[i].path));
            g_docs[i].token_count = 0;
            g_docs[i].link_count  = 0;
            g_doc_count++;
            if (g_docs_idx >= 0) {
                btree_insert(g_docs_idx, (int32_t)i,
                             (int32_t)g_docs[i].path_hash & 0x7fffffff);
            }
            return i;
        }
    }
    return -1;
}

static void nz_doc_free(int idx) {
    if (idx < 0 || idx >= NOTES_MAX_DOCS || !g_docs[idx].used) return;
    g_docs[idx].used = 0;
    if (g_total_tokens >= g_docs[idx].token_count)
        g_total_tokens -= g_docs[idx].token_count;
    if (g_total_backlinks >= g_docs[idx].link_count)
        g_total_backlinks -= g_docs[idx].link_count;
    g_docs[idx].token_count = 0;
    g_docs[idx].link_count  = 0;
    if (g_docs_idx >= 0) btree_delete(g_docs_idx, (int32_t)idx);
    if (g_doc_count > 0) g_doc_count--;
}

/* ── Wikilink extraction (regex \[\[([^\]]+)\]\]) ──────────────── */
/* State machine: scan for "[[", capture until "]]". Length-bounded.   */

static int nz_extract_links(const char *text, int len, int doc_idx,
                            uint32_t source_hash) {
    int found = 0;
    int i = 0;
    while (i + 3 < len) {
        if (text[i] == '[' && text[i + 1] == '[') {
            int start = i + 2;
            int j = start;
            while (j + 1 < len && !(text[j] == ']' && text[j + 1] == ']')
                   && text[j] != '\n' && (j - start) < NOTES_TOKEN_MAX)
                j++;
            if (j + 1 < len && text[j] == ']' && text[j + 1] == ']'
                && j > start) {
                /* Captured group: text[start..j) */
                int32_t key = nz_hash(text + start, j - start);
                if (g_backlinks >= 0) {
                    btree_insert(g_backlinks, key,
                                 (int32_t)(source_hash & 0x7fffffffu));
                    g_total_backlinks++;
                    if (doc_idx >= 0) g_docs[doc_idx].link_count++;
                    found++;
                }
                i = j + 2;
                continue;
            }
        }
        i++;
    }
    return found;
}

/* ── Tokenize for search index ─────────────────────────────────── */

static int nz_is_token_char(char c) {
    if (c >= 'a' && c <= 'z') return 1;
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= '0' && c <= '9') return 1;
    return c == '_';
}

static int nz_extract_tokens(const char *text, int len, int doc_idx) {
    int n = 0;
    int i = 0;
    char tok[NOTES_TOKEN_MAX];
    while (i < len) {
        while (i < len && !nz_is_token_char(text[i])) i++;
        int t = 0;
        while (i < len && nz_is_token_char(text[i]) && t < NOTES_TOKEN_MAX - 1) {
            tok[t++] = nz_tolower(text[i]);
            i++;
        }
        if (t < 2) continue;            /* skip 1-char tokens */
        tok[t] = 0;
        int32_t key = nz_hash(tok, t);
        if (g_search >= 0) {
            btree_insert(g_search, key, (int32_t)doc_idx);
            n++;
        }
    }
    if (doc_idx >= 0) g_docs[doc_idx].token_count += (uint32_t)n;
    g_total_tokens += (uint32_t)n;
    return n;
}

/* ── Read .md file via fat32 (editor writes there); fall back to
 *    vault if fat32 open fails. ─────────────────────────────────── */

static int nz_read_md(const char *path, char *buf, int max) {
    struct fat32_file f;
    if (fat32_mounted() && fat32_open(path, &f) == 0 && !f.is_dir) {
        int n = fat32_read(&f, buf, (uint32_t)(max - 1));
        if (n > 0) { buf[n] = 0; return n; }
    }
    int sz = vault_size(path);
    if (sz > 0) {
        int n = vault_read(path, buf, (uint32_t)(max - 1));
        if (n > 0) { buf[n] = 0; return n; }
    }
    return -1;
}

/* ── Index a single file. kind=0 -> remove; kind=1 -> add/update. ── */

int notes_zeos_index_file(const char *path, int kind) {
    notes_zeos_init();
    if (!path || !nz_is_md(path)) return 0;

    int idx = nz_doc_find(path);

    if (kind == 0) {
        if (idx >= 0) nz_doc_free(idx);
        return 0;
    }

    /* Re-resolve: drop the doc's prior contribution, re-add. */
    if (idx >= 0) {
        if (g_total_tokens >= g_docs[idx].token_count)
            g_total_tokens -= g_docs[idx].token_count;
        if (g_total_backlinks >= g_docs[idx].link_count)
            g_total_backlinks -= g_docs[idx].link_count;
        g_docs[idx].token_count = 0;
        g_docs[idx].link_count  = 0;
    } else {
        idx = nz_doc_alloc(path);
        if (idx < 0) return 0;
    }

    static char buf[NOTES_READ_BUF];
    int n = nz_read_md(path, buf, sizeof(buf));
    if (n <= 0) return 0;

    uint32_t source_hash = (uint32_t)nz_hash(path, nz_strlen(path));
    int links = nz_extract_links(buf, n, idx, source_hash);
    nz_extract_tokens(buf, n, idx);
    return links;
}

/* ── fs_event listener ─────────────────────────────────────────── */

static void nz_listener(const fs_event_t *e, void *ctx) {
    (void)ctx;
    if (!e || !e->valid) return;
    switch (e->kind) {
    case FS_EV_CREATE:
    case FS_EV_WRITE:
    case FS_EV_TRUNCATE:
    case FS_EV_RESTORE:
        if (nz_is_md(e->path))
            notes_zeos_index_file(e->path, 1);
        break;
    case FS_EV_UNLINK:
    case FS_EV_TRASH:
    case FS_EV_RMDIR:
        if (nz_is_md(e->path))
            notes_zeos_index_file(e->path, 0);
        break;
    case FS_EV_RENAME:
        if (nz_is_md(e->path))
            notes_zeos_index_file(e->path, 0);
        if (e->new_path[0] && nz_is_md(e->new_path))
            notes_zeos_index_file(e->new_path, 1);
        break;
    default:
        break;
    }
}

/* ── Init ──────────────────────────────────────────────────────── */

void notes_zeos_init(void) {
    if (g_initialized) return;
    btree_init();
    if (g_backlinks < 0) g_backlinks = btree_new();
    if (g_search    < 0) g_search    = btree_new();
    if (g_docs_idx  < 0) g_docs_idx  = btree_new();
    g_initialized = 1;

    if (!g_listener_on) {
        if (fs_event_register_listener(nz_listener, 0) >= 0)
            g_listener_on = 1;
    }
}

/* ── Query: backlinks ──────────────────────────────────────────── */

struct nz_visit_ctx {
    uint32_t want_value_hash;
    int      hits;
    int      print_paths;
};

static void nz_visit_backlinks(int32_t key, int32_t value, void *ud) {
    (void)key;
    struct nz_visit_ctx *c = (struct nz_visit_ctx *)ud;
    /* btree_insert overwrites, so range gives one value per key. We
     * scan all docs and check each doc's path_hash against the value
     * recorded against this key. The "many sources -> one target"
     * relation requires a second lookup: walk g_docs, for each doc
     * whose hash matches `value`, print its path. */
    for (int i = 0; i < NOTES_MAX_DOCS; i++) {
        if (!g_docs[i].used) continue;
        if ((int32_t)(g_docs[i].path_hash & 0x7fffffff) == value) {
            c->hits++;
            if (c->print_paths) {
                kputs("    ");
                kputs(g_docs[i].path);
                kputs("\n");
            }
        }
    }
}

int notes_zeos_print_backlinks(const char *target) {
    notes_zeos_init();
    if (!target || !*target) {
        kputs("  Usage: notes backlinks <path-or-name>\n");
        return 0;
    }
    /* Target key is hash of basename without the .md extension if
     * present, OR the raw arg as supplied — try both. */
    const char *base = nz_basename(target);
    int blen = nz_strlen(base);
    /* Strip .md for the wikilink convention [[Page]] not [[Page.md]]. */
    if (blen > 3
        && nz_tolower(base[blen - 3]) == '.'
        && nz_tolower(base[blen - 2]) == 'm'
        && nz_tolower(base[blen - 1]) == 'd')
        blen -= 3;

    int32_t k1 = nz_hash(base, blen);
    int32_t k2 = nz_hash(target, nz_strlen(target));

    kputs("  Pages linking to "); kputs(target); kputs(":\n");

    /* Walk all docs once: for each doc, the btree may have an entry
     * keyed on the target with value = source hash. We do this by
     * scanning the search/backlinks btree for both keys and
     * cross-referencing source hashes back to doc paths. */
    struct nz_visit_ctx c = { 0, 0, 1 };
    if (g_backlinks >= 0) {
        btree_range(g_backlinks, k1, k1, nz_visit_backlinks, &c);
        if (c.hits == 0 && k2 != k1)
            btree_range(g_backlinks, k2, k2, nz_visit_backlinks, &c);
    }
    if (c.hits == 0) kputs("    (none)\n");
    else { kputs("  "); kput_dec((unsigned)c.hits); kputs(" page(s)\n"); }
    return c.hits;
}

/* ── Query: search ─────────────────────────────────────────────── */

struct nz_search_ctx {
    int seen_doc[NOTES_MAX_DOCS];
    int hits;
};

static void nz_visit_search(int32_t key, int32_t value, void *ud) {
    (void)key;
    struct nz_search_ctx *c = (struct nz_search_ctx *)ud;
    if (value < 0 || value >= NOTES_MAX_DOCS) return;
    if (c->seen_doc[value]) return;
    c->seen_doc[value] = 1;
    if (g_docs[value].used) {
        kputs("    ");
        kputs(g_docs[value].path);
        kputs("\n");
        c->hits++;
    }
}

int notes_zeos_print_search(const char *query) {
    notes_zeos_init();
    if (!query || !*query) {
        kputs("  Usage: notes search <token>\n");
        return 0;
    }
    /* Tokenize the query: hits are docs containing ANY token. */
    static struct nz_search_ctx c;
    for (int i = 0; i < NOTES_MAX_DOCS; i++) c.seen_doc[i] = 0;
    c.hits = 0;
    int qlen = nz_strlen(query);
    int i = 0;
    char tok[NOTES_TOKEN_MAX];
    int any = 0;
    while (i < qlen) {
        while (i < qlen && !nz_is_token_char(query[i])) i++;
        int t = 0;
        while (i < qlen && nz_is_token_char(query[i])
               && t < NOTES_TOKEN_MAX - 1) {
            tok[t++] = nz_tolower(query[i]); i++;
        }
        if (t < 2) continue;
        tok[t] = 0;
        any = 1;
        int32_t k = nz_hash(tok, t);
        kputs("  query token: "); kputs(tok); kputs("\n");
        if (g_search >= 0)
            btree_range(g_search, k, k, nz_visit_search, &c);
    }
    if (!any) {
        kputs("  (no usable tokens in query)\n");
        return 0;
    }
    if (c.hits == 0) kputs("    (no matches)\n");
    else { kputs("  "); kput_dec((unsigned)c.hits); kputs(" doc(s)\n"); }
    return c.hits;
}

/* ── Stats ─────────────────────────────────────────────────────── */

void notes_zeos_print_stats(void) {
    notes_zeos_init();
    kputs("  notes-zeos index stats:\n");
    kputs("    docs       : ");
    kput_dec((unsigned)g_doc_count); kputs("\n");
    kputs("    tokens     : ");
    kput_dec(g_total_tokens); kputs("\n");
    kputs("    backlinks  : ");
    kput_dec(g_total_backlinks); kputs("\n");
    kputs("    listener   : ");
    kputs(g_listener_on ? "registered (fs_event)" : "(not registered)");
    kputs("\n");
    kputs("    btree h    : backlinks=");
    kput_dec((unsigned)g_backlinks); kputs(" search=");
    kput_dec((unsigned)g_search); kputs(" docs=");
    kput_dec((unsigned)g_docs_idx); kputs("\n");
}

uint32_t notes_zeos_total_docs(void)      { return (uint32_t)g_doc_count; }
uint32_t notes_zeos_total_tokens(void)    { return g_total_tokens; }
uint32_t notes_zeos_total_backlinks(void) { return g_total_backlinks; }

int notes_zeos_walk(void (*cb)(const char *path,
                                uint32_t token_count,
                                uint32_t link_count,
                                void *user),
                    void *user) {
    if (!cb) return 0;
    int n = 0;
    for (int i = 0; i < NOTES_MAX_DOCS; i++) {
        if (!g_docs[i].used) continue;
        cb(g_docs[i].path, g_docs[i].token_count, g_docs[i].link_count, user);
        n++;
    }
    return n;
}
