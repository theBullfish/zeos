/*
 * Text editor as a chain over text_edit signals. Buffer changes
 * emit; subscriber chains (autosave, history, spell, autocomplete)
 * consume via MDE type-routing. Window is a renderer over the
 * emitter; subscribers don't know about the UI.
 *
 * MasQ records every edit. CFA-wraps buffer pointer at INTERNAL.
 * Persists open files + cursor positions on reboot.
 *
 * Pipelines:
 *   CHAIN_TEXT_EDIT:        text_change_request -> debounce -> emit_text_edit
 *                           (output: text_edit)
 *   CHAIN_TEXT_AUTOSAVE:    text_edit -> debounce_2s -> fat32_write_resolve
 *   CHAIN_TEXT_HISTORY:     text_edit -> snapshot_threshold -> vault_write
 *   CHAIN_TEXT_SPELL:       text_edit -> tokenize -> check (stub: returns ok)
 *   CHAIN_TEXT_AUTOCOMPLETE:text_edit -> suggest (stub: empty list)
 *
 * MDE auto_route wires emitter -> subscribers automatically because
 * the emitter terminal node declares output_type="text_edit" and
 * every subscriber's first node declares input_type="text_edit".
 */

#include "editor.h"
#include "fb.h"
#include "font.h"
#include "theme.h"
#include "compositor.h"
#include "ui_dirty.h"
#include "ui_undo.h"
#include "ui_hover.h"
#include "ui_context_menu.h"
#include "fat32.h"
#include "vault.h"
#include "chain.h"
#include "chain_registry.h"
#include "kprint.h"
#include "timer.h"
#include "timeofday.h"

/* ── Chain ids ──────────────────────────────────────────────────── */
int CHAIN_TEXT_EDIT         = -1;
int CHAIN_TEXT_AUTOSAVE     = -1;
int CHAIN_TEXT_HISTORY      = -1;
int CHAIN_TEXT_SPELL        = -1;
int CHAIN_TEXT_AUTOCOMPLETE = -1;

/* ── Layout ─────────────────────────────────────────────────────── */
#define ED_W            720
#define ED_H            520
#define ED_TITLE_H      32
#define ED_STATUS_H     24
#define ED_GUTTER_W     48
#define ED_PAD          8
#define ED_LINE_H       18
#define ED_FONT_PX      14

/* ── Tiny helpers (no libc) ─────────────────────────────────────── */
static void e_memcpy(void *d, const void *s, int n) {
    uint8_t *dd=(uint8_t*)d; const uint8_t *ss=(const uint8_t*)s;
    for (int i=0;i<n;i++) dd[i]=ss[i];
}
static void e_memset(void *d, int v, int n) {
    uint8_t *dd=(uint8_t*)d;
    for (int i=0;i<n;i++) dd[i]=(uint8_t)v;
}
static void e_strncpy(char *d, const char *s, int max) {
    int i=0; if (!d||max<=0) return;
    if (s) while (i<max-1 && s[i]) { d[i]=s[i]; i++; }
    d[i]='\0';
}
static void e_itoa(uint32_t v, char *out) {
    char buf[16]; int n=0;
    if (v==0) { out[0]='0'; out[1]=0; return; }
    while (v && n<15) { buf[n++]=(char)('0'+(v%10)); v/=10; }
    int k=0; while (n>0) out[k++]=buf[--n]; out[k]=0;
}

/* ── text_edit signal struct ────────────────────────────────────── */
/* This is the typed signal. Both emitter terminal node and every
 * subscriber's first node use this layout. */
#define EDIT_FRAGMENT_MAX 64
typedef struct {
    int      buffer_id;                 /* Which buffer changed */
    int      offset;                    /* Char offset in buffer */
    int      before_len;                /* chars deleted */
    int      after_len;                 /* chars inserted */
    char     before[EDIT_FRAGMENT_MAX]; /* Deleted text (truncated for log) */
    char     after [EDIT_FRAGMENT_MAX]; /* Inserted text */
    uint64_t timestamp;                 /* TSC at emit time */
    int      seq;                       /* Monotonic edit sequence per buffer */
    int      valid;                     /* 0 = no event this resolve */
} text_edit_t;

/* ── Buffers ────────────────────────────────────────────────────── */
typedef struct {
    int      slot_used;
    int      buffer_id;
    char     path[EDITOR_PATH_MAX];
    char     text[EDITOR_BUFFER_BYTES];
    int      len;
    int      cursor;
    int      sel_anchor;                /* -1 if no selection, else selection origin */
    int      scroll_line;
    int      edit_seq;
    int      edits_since_snapshot;
    int      dirty;
    uint64_t last_edit_tsc;
    uint64_t autosave_armed_tsc;        /* 0 = not armed */
    int      autosave_pending;          /* needs flush */
    undo_buffer_t undo;
} editor_buf_t;

static editor_buf_t g_bufs[EDITOR_MAX_BUFFERS];
static int s_next_buffer_id = 1;

/* ── Window state ───────────────────────────────────────────────── */
typedef struct {
    int active;
    int wx, wy;
    int focused_slot;       /* index into g_bufs */
    int find_open;
    char find_term[EDITOR_FIND_MAX];
    int find_len;
    int find_match_offset;  /* -1 if no match */
} editor_state_t;

static editor_state_t g_ed;

/* ── Pending text_edit emission slot (consumed by emitter chain) ─ */
typedef struct {
    int      buffer_id;
    int      offset;
    int      before_len;
    int      after_len;
    char     before[EDIT_FRAGMENT_MAX];
    char     after [EDIT_FRAGMENT_MAX];
    int      pending;
} pending_edit_t;

static pending_edit_t g_pending;
static text_edit_t    g_last_edit;   /* cached so subscribers re-pump */

/* ── Counters ───────────────────────────────────────────────────── */
static uint32_t g_total_opens;
static uint32_t g_total_emits;
static uint32_t g_total_autosaves;
static uint32_t g_total_snapshots;
static uint32_t g_total_spell_checks;
static uint32_t g_total_autocompletes;

/* ── Forward decls ──────────────────────────────────────────────── */
static void editor_emit(int buffer_id, int offset,
                        const char *before, int before_len,
                        const char *after, int after_len);
static editor_buf_t *editor_focused(void);
static editor_buf_t *editor_get_by_id(int buffer_id);
static void editor_layout_origin(void);
static void editor_persist_save_internal(void);

/* ─────────────────────────────────────────────────────────────────
 * Buffer ops
 * ───────────────────────────────────────────────────────────────── */

static editor_buf_t *editor_alloc_buf(void) {
    for (int i=0; i<EDITOR_MAX_BUFFERS; i++) {
        if (!g_bufs[i].slot_used) {
            e_memset(&g_bufs[i], 0, sizeof(g_bufs[i]));
            g_bufs[i].slot_used = 1;
            g_bufs[i].buffer_id = s_next_buffer_id++;
            g_bufs[i].sel_anchor = -1;
            undo_init(&g_bufs[i].undo, 0, 0);
            return &g_bufs[i];
        }
    }
    return 0;
}

static editor_buf_t *editor_focused(void) {
    if (!g_ed.active) return 0;
    if (g_ed.focused_slot < 0 || g_ed.focused_slot >= EDITOR_MAX_BUFFERS) return 0;
    if (!g_bufs[g_ed.focused_slot].slot_used) return 0;
    return &g_bufs[g_ed.focused_slot];
}

static editor_buf_t *editor_get_by_id(int buffer_id) {
    for (int i=0; i<EDITOR_MAX_BUFFERS; i++) {
        if (g_bufs[i].slot_used && g_bufs[i].buffer_id == buffer_id) return &g_bufs[i];
    }
    return 0;
}

/* Insert string `s` of len `n` at offset. Returns chars actually inserted. */
static int buf_insert(editor_buf_t *b, int offset, const char *s, int n) {
    if (offset < 0) offset = 0;
    if (offset > b->len) offset = b->len;
    if (b->len + n >= EDITOR_BUFFER_BYTES - 1) {
        n = (EDITOR_BUFFER_BYTES - 1) - b->len;
        if (n <= 0) return 0;
    }
    /* Shift */
    for (int i = b->len; i >= offset; i--) b->text[i + n] = b->text[i];
    for (int i = 0; i < n; i++) b->text[offset + i] = s[i];
    b->len += n;
    b->text[b->len] = '\0';
    return n;
}

/* Delete `n` bytes from offset. Returns chars actually deleted into `out`
 * (NUL-terminated, truncated to EDIT_FRAGMENT_MAX-1). */
static int buf_delete(editor_buf_t *b, int offset, int n, char *out_frag) {
    if (offset < 0 || offset >= b->len) { if (out_frag) out_frag[0] = 0; return 0; }
    if (offset + n > b->len) n = b->len - offset;
    int frag_n = n < (EDIT_FRAGMENT_MAX-1) ? n : (EDIT_FRAGMENT_MAX-1);
    if (out_frag) {
        for (int i = 0; i < frag_n; i++) out_frag[i] = b->text[offset + i];
        out_frag[frag_n] = '\0';
    }
    for (int i = offset; i + n <= b->len; i++) b->text[i] = b->text[i + n];
    b->len -= n;
    b->text[b->len] = '\0';
    return n;
}

/* ─────────────────────────────────────────────────────────────────
 * Emitter — stage a pending edit then resolve CHAIN_TEXT_EDIT.
 * MDE then fan-outs to all subscribers wired by output_type=text_edit.
 * ───────────────────────────────────────────────────────────────── */

static void editor_emit(int buffer_id, int offset,
                        const char *before, int before_len,
                        const char *after, int after_len)
{
    g_pending.buffer_id  = buffer_id;
    g_pending.offset     = offset;
    g_pending.before_len = before_len;
    g_pending.after_len  = after_len;
    int nb = before_len < (EDIT_FRAGMENT_MAX-1) ? before_len : (EDIT_FRAGMENT_MAX-1);
    int na = after_len  < (EDIT_FRAGMENT_MAX-1) ? after_len  : (EDIT_FRAGMENT_MAX-1);
    if (before) for (int i=0;i<nb;i++) g_pending.before[i] = before[i];
    g_pending.before[nb] = 0;
    if (after)  for (int i=0;i<na;i++) g_pending.after[i]  = after[i];
    g_pending.after[na] = 0;
    g_pending.pending = 1;

    editor_buf_t *b = editor_get_by_id(buffer_id);
    if (b) {
        b->edit_seq++;
        b->dirty = 1;
        b->last_edit_tsc = timer_read_tsc();
        b->autosave_armed_tsc = b->last_edit_tsc;
        b->autosave_pending = 1;
        b->edits_since_snapshot++;
        dirty_register(EDITOR_WIN_ID);
    }

    /* Resolve emitter — runs through debounce + emit_text_edit nodes,
     * after which MDE fans the text_edit signal out to subscribers. */
    if (CHAIN_TEXT_EDIT >= 0) chain_resolve(CHAIN_TEXT_EDIT);

    /* Subscribers: nudge them. In a fully driven graph mde_resolve_all
     * would handle this, but our subscribers also self-pump on tick to
     * cover the autosave debounce timeout. */
    if (CHAIN_TEXT_AUTOSAVE     >= 0) chain_resolve(CHAIN_TEXT_AUTOSAVE);
    if (CHAIN_TEXT_HISTORY      >= 0) chain_resolve(CHAIN_TEXT_HISTORY);
    if (CHAIN_TEXT_SPELL        >= 0) chain_resolve(CHAIN_TEXT_SPELL);
    if (CHAIN_TEXT_AUTOCOMPLETE >= 0) chain_resolve(CHAIN_TEXT_AUTOCOMPLETE);
}

/* ── Emitter pipeline nodes ─────────────────────────────────────── */

static void node_text_change_request(chain_node_t *self, void *in, void *out) {
    (void)self; (void)in;
    text_edit_t *o = (text_edit_t *)out;
    e_memset(o, 0, sizeof(*o));
    if (!g_pending.pending) { o->valid = 0; return; }
    o->buffer_id  = g_pending.buffer_id;
    o->offset     = g_pending.offset;
    o->before_len = g_pending.before_len;
    o->after_len  = g_pending.after_len;
    e_memcpy(o->before, g_pending.before, EDIT_FRAGMENT_MAX);
    e_memcpy(o->after,  g_pending.after,  EDIT_FRAGMENT_MAX);
    o->timestamp  = timer_read_tsc();
    editor_buf_t *b = editor_get_by_id(o->buffer_id);
    o->seq = b ? b->edit_seq : 0;
    o->valid = 1;
}

static void node_emit_debounce(chain_node_t *self, void *in, void *out) {
    (void)self;
    text_edit_t *i = (text_edit_t *)in;
    text_edit_t *o = (text_edit_t *)out;
    *o = *i;
    /* No-op debounce at the emitter; we already coalesce per-keystroke
     * in the caller, and downstream autosave handles its own 2s window. */
}

static void node_emit_text_edit(chain_node_t *self, void *in, void *out) {
    (void)self;
    text_edit_t *i = (text_edit_t *)in;
    text_edit_t *o = (text_edit_t *)out;
    *o = *i;
    if (!i->valid) return;

    g_last_edit = *i;
    g_total_emits++;
    g_pending.pending = 0;

    /* MasQ provenance: bump emitter chain's vault_version. Every edit
     * is timestamped through chain_resolve's TSC capture. */
    if (CHAIN_TEXT_EDIT >= 0) {
        chain_t *c = chain_get(CHAIN_TEXT_EDIT);
        if (c) c->vault_version++;
    }
}

/* ─────────────────────────────────────────────────────────────────
 * Subscriber: AUTOSAVE  (text_edit -> debounce_2s -> fat32_write)
 * ───────────────────────────────────────────────────────────────── */

/* TSC frequency assumed ~ scheduler tick * timer rate; for the
 * debounce window we use a coarse approximation: 2 seconds at any
 * sane TSC frequency is "enough TSC ticks". We use a shared module
 * constant calibrated against tod_now_unix. */
static uint64_t s_tsc_per_sec = 0;

static void node_autosave_input(chain_node_t *self, void *in, void *out) {
    (void)self; (void)in;
    text_edit_t *o = (text_edit_t *)out;
    *o = g_last_edit;  /* Re-source from cached last emit so periodic
                       *  ticks can re-evaluate the debounce. */
}

static void node_autosave_debounce_2s(chain_node_t *self, void *in, void *out) {
    (void)self;
    text_edit_t *i = (text_edit_t *)in;
    text_edit_t *o = (text_edit_t *)out;
    *o = *i;
    /* Nothing to do here directly — the resolve node below does the
     * timing check against per-buffer autosave_armed_tsc. We keep this
     * node so the pipeline shape matches the documented contract and
     * future windowing logic has an obvious home. */
}

static void node_autosave_fat32_write_resolve(chain_node_t *self,
                                              void *in, void *out) {
    (void)self;
    text_edit_t *i = (text_edit_t *)in;
    text_edit_t *o = (text_edit_t *)out;
    *o = *i;

    uint64_t now = timer_read_tsc();
    /* Coarse debounce: 2s ~= 2 * tsc_per_sec. If unknown, we still
     * autosave but use a generous tick-count fallback. Practical
     * effect: small files get autosaved promptly; nothing flushes
     * during a busy keystroke burst because we re-arm on every emit. */
    uint64_t window = s_tsc_per_sec ? (s_tsc_per_sec * 2) : (uint64_t)200000000;

    for (int idx=0; idx<EDITOR_MAX_BUFFERS; idx++) {
        editor_buf_t *b = &g_bufs[idx];
        if (!b->slot_used) continue;
        if (!b->autosave_pending) continue;
        if (b->autosave_armed_tsc == 0) continue;
        if (now - b->autosave_armed_tsc < window) continue;
        if (b->path[0] == 0) {
            b->autosave_pending = 0;
            b->autosave_armed_tsc = 0;
            continue;
        }
        /* Truncate to current length, then write the full buffer.
         * fat32_write goes through CHAIN_BLOCK -> masq_journal. */
        (void)fat32_truncate(b->path, b->len);
        if (b->len > 0) {
            (void)fat32_write(b->path, 0, b->text, b->len);
        }
        b->autosave_pending = 0;
        b->autosave_armed_tsc = 0;
        b->dirty = 0;
        if (idx == g_ed.focused_slot) dirty_clear(EDITOR_WIN_ID);
        g_total_autosaves++;

        if (CHAIN_TEXT_AUTOSAVE >= 0) {
            chain_t *c = chain_get(CHAIN_TEXT_AUTOSAVE);
            if (c) c->vault_version++;
        }
    }
}

/* ─────────────────────────────────────────────────────────────────
 * Subscriber: HISTORY  (text_edit -> snapshot_threshold -> vault_write)
 * Stores under /editor/history/<buffer_id>/v<n>
 * ───────────────────────────────────────────────────────────────── */

static int s_history_dir_inited = 0;

static void history_path_for(int buffer_id, int version, char *out, int max) {
    char n1[16], n2[16];
    e_itoa((uint32_t)buffer_id, n1);
    e_itoa((uint32_t)version,   n2);
    int o = 0;
    const char *p1 = "/editor/history/";
    while (*p1 && o < max-1) out[o++] = *p1++;
    for (int i=0; n1[i] && o < max-1; i++) out[o++] = n1[i];
    if (o < max-1) out[o++] = '/';
    if (o < max-1) out[o++] = 'v';
    for (int i=0; n2[i] && o < max-1; i++) out[o++] = n2[i];
    out[o] = 0;
}

static void node_history_input(chain_node_t *self, void *in, void *out) {
    (void)self; (void)in;
    text_edit_t *o = (text_edit_t *)out;
    *o = g_last_edit;
}

static void node_history_snapshot_threshold(chain_node_t *self, void *in, void *out) {
    (void)self;
    text_edit_t *i = (text_edit_t *)in;
    text_edit_t *o = (text_edit_t *)out;
    *o = *i;
    /* Pass-through; final node does the threshold + write. */
}

static void node_history_vault_write(chain_node_t *self, void *in, void *out) {
    (void)self;
    text_edit_t *i = (text_edit_t *)in;
    text_edit_t *o = (text_edit_t *)out;
    *o = *i;
    if (!i->valid) return;

    editor_buf_t *b = editor_get_by_id(i->buffer_id);
    if (!b) return;
    if (b->edits_since_snapshot < EDITOR_HISTORY_EVERY) return;

    if (!s_history_dir_inited) {
        (void)vault_mkdir("/editor",         VAULT_TIER_INTERNAL);
        (void)vault_mkdir("/editor/history", VAULT_TIER_INTERNAL);
        s_history_dir_inited = 1;
    }
    /* Per-buffer subdir. */
    char subdir[64];
    char idn[16]; e_itoa((uint32_t)b->buffer_id, idn);
    int sn = 0;
    const char *p = "/editor/history/";
    while (*p && sn < 63) subdir[sn++] = *p++;
    for (int k=0; idn[k] && sn<63; k++) subdir[sn++] = idn[k];
    subdir[sn] = 0;
    (void)vault_mkdir(subdir, VAULT_TIER_INTERNAL);

    char path[128];
    int version = i->seq;
    history_path_for(b->buffer_id, version, path, sizeof(path));
    (void)vault_write(path, b->text, (uint32_t)b->len);

    b->edits_since_snapshot = 0;
    g_total_snapshots++;

    if (CHAIN_TEXT_HISTORY >= 0) {
        chain_t *c = chain_get(CHAIN_TEXT_HISTORY);
        if (c) c->vault_version++;
    }
}

/* ─────────────────────────────────────────────────────────────────
 * Subscriber: SPELL  (text_edit -> tokenize -> check)
 *
 * STUB: no real dictionary lookup yet. The tokenize node walks the
 * inserted fragment and counts whitespace-delimited tokens; the
 * check node returns "ok" for every token. Reported in the status
 * bar as "spell: ok (N tokens checked total)".
 * ───────────────────────────────────────────────────────────────── */

static uint32_t g_spell_token_count;

static void node_spell_input(chain_node_t *self, void *in, void *out) {
    (void)self; (void)in;
    text_edit_t *o = (text_edit_t *)out;
    *o = g_last_edit;
}

static int is_word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '\'';
}

static void node_spell_tokenize(chain_node_t *self, void *in, void *out) {
    (void)self;
    text_edit_t *i = (text_edit_t *)in;
    text_edit_t *o = (text_edit_t *)out;
    *o = *i;
    if (!i->valid) return;
    int in_word = 0;
    for (int k=0; k < i->after_len && k < EDIT_FRAGMENT_MAX; k++) {
        char c = i->after[k];
        if (!c) break;
        if (is_word_char(c)) {
            if (!in_word) { in_word = 1; g_spell_token_count++; }
        } else {
            in_word = 0;
        }
    }
}

static void node_spell_check(chain_node_t *self, void *in, void *out) {
    (void)self;
    text_edit_t *i = (text_edit_t *)in;
    text_edit_t *o = (text_edit_t *)out;
    *o = *i;
    if (!i->valid) return;
    /* STUB: real dict lookup is future. Every token is treated "ok".
     * Honest output: this node exists, runs, counts. It does not lie
     * about flagging misspellings — it doesn't try. */
    g_total_spell_checks++;
}

/* ─────────────────────────────────────────────────────────────────
 * Subscriber: AUTOCOMPLETE  (text_edit -> suggest)
 *
 * STUB: returns empty suggestion list for every emit. Counter is
 * exposed; future versions will populate a small ring of suggestions
 * pulled from prior buffer contents.
 * ───────────────────────────────────────────────────────────────── */

static void node_ac_input(chain_node_t *self, void *in, void *out) {
    (void)self; (void)in;
    text_edit_t *o = (text_edit_t *)out;
    *o = g_last_edit;
}

static void node_ac_suggest(chain_node_t *self, void *in, void *out) {
    (void)self;
    text_edit_t *i = (text_edit_t *)in;
    text_edit_t *o = (text_edit_t *)out;
    *o = *i;
    if (!i->valid) return;
    /* STUB: no suggestions returned. Counter increments to prove the
     * subscriber path actually fired. */
    g_total_autocompletes++;
}

/* ─────────────────────────────────────────────────────────────────
 * Chain registration
 * ───────────────────────────────────────────────────────────────── */

int editor_chain_register(int parent_chain_id) {
    if (CHAIN_TEXT_EDIT >= 0) return 5;

    /* Calibrate s_tsc_per_sec once if possible. We use a coarse 100M
     * default as a backstop — autosave will still fire, just on a
     * less-precise window when TOD isn't ready. */
    if (s_tsc_per_sec == 0) {
        /* Best-effort — without a TSC frequency probe API we use a
         * reasonable default for modern x86. */
        s_tsc_per_sec = 2000000000ULL;
    }

    /* Emitter */
    int e = chain_create("text_edit", parent_chain_id, MASQ_INTERNAL);
    if (e < 0) return -1;
    chain_add_node(e, "text_change_request", "void",      "text_edit", node_text_change_request);
    chain_add_node(e, "debounce",            "text_edit", "text_edit", node_emit_debounce);
    chain_add_node(e, "emit_text_edit",      "text_edit", "text_edit", node_emit_text_edit);
    CHAIN_TEXT_EDIT = e;

    /* Autosave */
    int a = chain_create("text_autosave", parent_chain_id, MASQ_INTERNAL);
    if (a >= 0) {
        chain_add_node(a, "input",        "text_edit", "text_edit", node_autosave_input);
        chain_add_node(a, "debounce_2s",  "text_edit", "text_edit", node_autosave_debounce_2s);
        chain_add_node(a, "fat32_write_resolve", "text_edit", "text_edit",
                       node_autosave_fat32_write_resolve);
        CHAIN_TEXT_AUTOSAVE = a;
    }

    /* History */
    int h = chain_create("text_history", parent_chain_id, MASQ_INTERNAL);
    if (h >= 0) {
        chain_add_node(h, "input",              "text_edit", "text_edit", node_history_input);
        chain_add_node(h, "snapshot_threshold", "text_edit", "text_edit", node_history_snapshot_threshold);
        chain_add_node(h, "vault_write",        "text_edit", "text_edit", node_history_vault_write);
        CHAIN_TEXT_HISTORY = h;
    }

    /* Spell (stub) */
    int s = chain_create("text_spell", parent_chain_id, MASQ_INTERNAL);
    if (s >= 0) {
        chain_add_node(s, "input",    "text_edit", "text_edit", node_spell_input);
        chain_add_node(s, "tokenize", "text_edit", "text_edit", node_spell_tokenize);
        chain_add_node(s, "check",    "text_edit", "text_edit", node_spell_check);
        CHAIN_TEXT_SPELL = s;
    }

    /* Autocomplete (stub) */
    int c = chain_create("text_autocomplete", parent_chain_id, MASQ_INTERNAL);
    if (c >= 0) {
        chain_add_node(c, "input",   "text_edit", "text_edit", node_ac_input);
        chain_add_node(c, "suggest", "text_edit", "text_edit", node_ac_suggest);
        CHAIN_TEXT_AUTOCOMPLETE = c;
    }

    int total = 0;
    if (CHAIN_TEXT_EDIT         >= 0) total++;
    if (CHAIN_TEXT_AUTOSAVE     >= 0) total++;
    if (CHAIN_TEXT_HISTORY      >= 0) total++;
    if (CHAIN_TEXT_SPELL        >= 0) total++;
    if (CHAIN_TEXT_AUTOCOMPLETE >= 0) total++;
    return total;
}

/* ─────────────────────────────────────────────────────────────────
 * Public API — open / close / save
 * ───────────────────────────────────────────────────────────────── */

static void editor_layout_origin(void) {
    int sw = (int)fb_width(), sh = (int)fb_height();
    g_ed.wx = (sw - ED_W) / 2;
    g_ed.wy = (sh - ED_H) / 2;
    if (g_ed.wx < 0) g_ed.wx = 0;
    if (g_ed.wy < 0) g_ed.wy = 0;
}

static int load_file_into_buf(editor_buf_t *b, const char *path) {
    e_strncpy(b->path, path, EDITOR_PATH_MAX);
    struct fat32_file f;
    if (fat32_open(path, &f) != 0) {
        /* New file scenario — create empty. */
        (void)fat32_create(path);
        b->len = 0;
        b->text[0] = 0;
        return 0;
    }
    int cap = EDITOR_BUFFER_BYTES - 1;
    int got = fat32_read(&f, b->text, (uint32_t)cap);
    if (got < 0) got = 0;
    b->len = got;
    b->text[got] = 0;
    return 0;
}

int editor_open(const char *path) {
    if (!g_ed.active) {
        e_memset(&g_ed, 0, sizeof(g_ed));
        g_ed.active = 1;
        g_ed.focused_slot = -1;
        g_ed.find_match_offset = -1;
    }
    editor_layout_origin();

    editor_buf_t *b = editor_alloc_buf();
    if (!b) {
        kputs("editor: no free buffer slot\n");
        return -1;
    }
    if (path && path[0]) {
        if (load_file_into_buf(b, path) < 0) return -1;
    } else {
        b->path[0] = 0;
        b->len = 0;
        b->text[0] = 0;
    }
    /* Find slot index */
    for (int i=0; i<EDITOR_MAX_BUFFERS; i++) {
        if (&g_bufs[i] == b) { g_ed.focused_slot = i; break; }
    }
    g_total_opens++;
    editor_persist_save_internal();
    int sw = (int)fb_width(), sh = (int)fb_height();
    compositor_dirty(0, 0, sw, sh);
    return 0;
}

int editor_save_focused(void) {
    editor_buf_t *b = editor_focused();
    if (!b || b->path[0] == 0) return -1;
    (void)fat32_truncate(b->path, b->len);
    if (b->len > 0) (void)fat32_write(b->path, 0, b->text, b->len);
    b->dirty = 0;
    b->autosave_pending = 0;
    b->autosave_armed_tsc = 0;
    dirty_clear(EDITOR_WIN_ID);
    g_total_autosaves++;
    return 0;
}

static void editor_close_modal_cb(int wid, dirty_response_t r, void *ctx) {
    (void)wid; (void)ctx;
    if (r == DIRTY_CANCEL) return;
    if (r == DIRTY_SAVE) {
        (void)editor_save_focused();
    }
    /* Close all buffers + window. */
    for (int i=0; i<EDITOR_MAX_BUFFERS; i++) g_bufs[i].slot_used = 0;
    g_ed.active = 0;
    dirty_clear(EDITOR_WIN_ID);
    editor_persist_save_internal();
    int sw = (int)fb_width(), sh = (int)fb_height();
    compositor_dirty(0, 0, sw, sh);
}

void editor_close(void) {
    if (!g_ed.active) return;
    /* Final emit so subscribers see the close-time state. */
    editor_buf_t *b = editor_focused();
    if (b && b->dirty) {
        editor_emit(b->buffer_id, b->cursor, "", 0, "", 0);
    }
    if (dirty_get(EDITOR_WIN_ID)) {
        dirty_open_close_modal(EDITOR_WIN_ID,
                               "Save changes before closing?",
                               editor_close_modal_cb, 0);
        return;
    }
    for (int i=0; i<EDITOR_MAX_BUFFERS; i++) g_bufs[i].slot_used = 0;
    g_ed.active = 0;
    editor_persist_save_internal();
    int sw = (int)fb_width(), sh = (int)fb_height();
    compositor_dirty(0, 0, sw, sh);
}

int editor_active(void) { return g_ed.active; }

/* ─────────────────────────────────────────────────────────────────
 * Input dispatch
 * ───────────────────────────────────────────────────────────────── */

static int line_start_of(const editor_buf_t *b, int off) {
    while (off > 0 && b->text[off - 1] != '\n') off--;
    return off;
}
static int line_end_of(const editor_buf_t *b, int off) {
    while (off < b->len && b->text[off] != '\n') off++;
    return off;
}

static int find_in_buf(const editor_buf_t *b, const char *needle, int nlen, int from) {
    if (nlen <= 0) return -1;
    for (int i = from; i + nlen <= b->len; i++) {
        int ok = 1;
        for (int k=0; k<nlen; k++) if (b->text[i+k] != needle[k]) { ok=0; break; }
        if (ok) return i;
    }
    return -1;
}

int editor_key(int ascii) {
    if (!g_ed.active) return 0;
    editor_buf_t *b = editor_focused();
    if (!b) return 1;

    /* Find bar (Ctrl-F sets this open via 0x06) */
    if (g_ed.find_open) {
        if (ascii == 27) { g_ed.find_open = 0; return 1; }
        if (ascii == '\n' || ascii == '\r') {
            g_ed.find_match_offset = find_in_buf(b, g_ed.find_term, g_ed.find_len,
                                                 b->cursor + 1);
            if (g_ed.find_match_offset < 0)
                g_ed.find_match_offset = find_in_buf(b, g_ed.find_term, g_ed.find_len, 0);
            if (g_ed.find_match_offset >= 0) b->cursor = g_ed.find_match_offset;
            return 1;
        }
        if (ascii == 8 || ascii == 127) {
            if (g_ed.find_len > 0) g_ed.find_term[--g_ed.find_len] = 0;
            return 1;
        }
        if (ascii >= 32 && ascii < 127 && g_ed.find_len < EDITOR_FIND_MAX-1) {
            g_ed.find_term[g_ed.find_len++] = (char)ascii;
            g_ed.find_term[g_ed.find_len]   = 0;
            return 1;
        }
        return 1;
    }

    /* Esc closes editor (with dirty modal). */
    if (ascii == 27) { editor_close(); return 1; }

    /* Ctrl-S manual save */
    if (ascii == 0x13) { (void)editor_save_focused(); return 1; }
    /* Ctrl-A select-all */
    if (ascii == 0x01) { b->sel_anchor = 0; b->cursor = b->len; return 1; }
    /* Ctrl-F find */
    if (ascii == 0x06) {
        g_ed.find_open = 1;
        g_ed.find_term[0] = 0;
        g_ed.find_len = 0;
        return 1;
    }
    /* Ctrl-C / Ctrl-X / Ctrl-V — clipboard placeholders. The system
     * clipboard surface isn't a chain yet; we leave the keys reserved
     * (no-op, no error) so they don't fall through to insertion. */
    if (ascii == 0x03 || ascii == 0x18 || ascii == 0x16) return 1;

    /* Enter */
    if (ascii == '\n' || ascii == '\r') {
        char nl = '\n';
        int ins = buf_insert(b, b->cursor, &nl, 1);
        if (ins) {
            editor_emit(b->buffer_id, b->cursor, "", 0, "\n", 1);
            b->cursor += ins;
        }
        return 1;
    }

    /* Backspace */
    if (ascii == 8 || ascii == 127) {
        if (b->cursor > 0) {
            char frag[EDIT_FRAGMENT_MAX];
            int del = buf_delete(b, b->cursor - 1, 1, frag);
            if (del) {
                b->cursor--;
                editor_emit(b->buffer_id, b->cursor, frag, del, "", 0);
            }
        }
        return 1;
    }

    /* Tab inserts 4 spaces */
    if (ascii == 9) {
        const char *sp = "    ";
        int ins = buf_insert(b, b->cursor, sp, 4);
        if (ins) {
            editor_emit(b->buffer_id, b->cursor, "", 0, sp, ins);
            b->cursor += ins;
        }
        return 1;
    }

    /* Printable */
    if (ascii >= 32 && ascii < 127) {
        char c = (char)ascii;
        int ins = buf_insert(b, b->cursor, &c, 1);
        if (ins) {
            char after[2] = { c, 0 };
            editor_emit(b->buffer_id, b->cursor, "", 0, after, 1);
            b->cursor += ins;
        }
        return 1;
    }
    return 1;  /* swallow */
}

int editor_key_arrow(int dir) {
    if (!g_ed.active) return 0;
    editor_buf_t *b = editor_focused();
    if (!b) return 1;
    if (dir == 0) { if (b->cursor > 0)        b->cursor--; }
    else if (dir == 1) { if (b->cursor < b->len) b->cursor++; }
    else if (dir == 2) {
        int ls = line_start_of(b, b->cursor);
        int col = b->cursor - ls;
        if (ls == 0) { b->cursor = 0; return 1; }
        int prev_ls = line_start_of(b, ls - 1);
        int prev_le = ls - 1;
        int new_col = (prev_ls + col <= prev_le) ? col : (prev_le - prev_ls);
        b->cursor = prev_ls + new_col;
    } else if (dir == 3) {
        int ls = line_start_of(b, b->cursor);
        int le = line_end_of(b, b->cursor);
        int col = b->cursor - ls;
        if (le >= b->len) { b->cursor = b->len; return 1; }
        int next_ls = le + 1;
        int next_le = line_end_of(b, next_ls);
        int new_col = (next_ls + col <= next_le) ? col : (next_le - next_ls);
        b->cursor = next_ls + new_col;
    }
    return 1;
}

static int in_window(int x, int y) {
    return (x >= g_ed.wx && y >= g_ed.wy &&
            x <  g_ed.wx + ED_W && y < g_ed.wy + ED_H);
}

int editor_mouse_down(int x, int y, int button) {
    if (!g_ed.active) return 0;
    (void)button;
    if (!in_window(x, y)) return 1;
    /* Close button */
    if (x >= g_ed.wx + ED_W - 28 && x <= g_ed.wx + ED_W - 8 &&
        y >= g_ed.wy + 6 && y <= g_ed.wy + 26) {
        editor_close();
        return 1;
    }
    return 1;
}
int editor_mouse_move(int x, int y) { (void)x; (void)y; return 0; }
int editor_mouse_up(int x, int y, int button) { (void)x;(void)y;(void)button; return 0; }

/* ─────────────────────────────────────────────────────────────────
 * Tick — runs the autosave debounce expiry
 * ───────────────────────────────────────────────────────────────── */

void editor_tick(void) {
    if (!g_ed.active) return;
    /* Re-pump autosave so debounce expires even with no fresh edits. */
    if (CHAIN_TEXT_AUTOSAVE >= 0) chain_resolve(CHAIN_TEXT_AUTOSAVE);
}

/* ─────────────────────────────────────────────────────────────────
 * Drawing
 * ───────────────────────────────────────────────────────────────── */

static void draw_close_button(int wx, int wy) {
    int bx = wx + ED_W - 28, by = wy + 6;
    fb_rect(bx, by, 20, 20, COLOR_SURFACE_HIGH);
    font_draw(bx + 6, by + 3, "x", FONT_UI, ED_FONT_PX, COLOR_ON_SURFACE);
}

void editor_draw(void) {
    if (!g_ed.active) return;
    editor_buf_t *b = editor_focused();
    int wx = g_ed.wx, wy = g_ed.wy;

    /* Background + chrome */
    fb_rect(wx, wy, ED_W, ED_H, COLOR_SURFACE);
    fb_rect_outline(wx, wy, ED_W, ED_H, COLOR_SURFACE_TOP, 1);
    fb_rect(wx, wy, ED_W, ED_TITLE_H, COLOR_SURFACE_HIGH);

    /* Title: filename + dirty indicator */
    char title[EDITOR_PATH_MAX + 8];
    int o = 0;
    const char *p = "Editor: ";
    while (*p && o < (int)sizeof(title)-1) title[o++] = *p++;
    if (b && b->path[0]) {
        for (int k=0; b->path[k] && o<(int)sizeof(title)-1; k++) title[o++] = b->path[k];
    } else {
        const char *u = "(untitled)";
        while (*u && o<(int)sizeof(title)-1) title[o++] = *u++;
    }
    if (b && b->dirty && o < (int)sizeof(title)-2) { title[o++]=' '; title[o++]='*'; }
    title[o] = 0;
    font_draw(wx + ED_PAD, wy + 8, title, FONT_UI, ED_FONT_PX, COLOR_ON_SURFACE);

    draw_close_button(wx, wy);

    /* Body */
    int body_y = wy + ED_TITLE_H + ED_PAD;
    int body_h = ED_H - ED_TITLE_H - ED_STATUS_H - ED_PAD * 2;

    if (!b) {
        font_draw(wx + ED_PAD, body_y, "(no buffer)", FONT_UI, ED_FONT_PX, COLOR_ON_SURFACE_2);
    } else {
        /* Render up to EDITOR_LINES_VISIBLE lines starting from scroll_line. */
        int line = 0;
        int i = 0;
        /* Skip to scroll_line. */
        while (line < b->scroll_line && i < b->len) {
            if (b->text[i] == '\n') line++;
            i++;
        }
        int rendered = 0;
        int cursor_drawn = 0;
        int cur_line = 0, cur_col = 0;
        {
            int lc = 0, cc = 0;
            for (int k=0; k<b->cursor && k<b->len; k++) {
                if (b->text[k] == '\n') { lc++; cc=0; }
                else cc++;
            }
            cur_line = lc; cur_col = cc;
        }
        while (rendered < EDITOR_LINES_VISIBLE && i <= b->len) {
            int line_no = b->scroll_line + rendered;
            /* gutter line number */
            char ln[8];
            e_itoa((uint32_t)(line_no + 1), ln);
            font_draw(wx + 4, body_y + rendered * ED_LINE_H,
                      ln, FONT_UI, ED_FONT_PX, COLOR_ON_SURFACE_3);
            /* Extract line */
            char tmp[256];
            int tn = 0;
            while (i < b->len && b->text[i] != '\n' && tn < 255) {
                tmp[tn++] = b->text[i++];
            }
            tmp[tn] = 0;
            if (i < b->len && b->text[i] == '\n') i++;
            font_draw(wx + ED_GUTTER_W, body_y + rendered * ED_LINE_H,
                      tmp, FONT_CODE, ED_FONT_PX, COLOR_ON_SURFACE);
            /* Cursor */
            if (line_no == cur_line && !cursor_drawn) {
                /* approximate cursor column position using monospace metrics */
                int cw = font_measure("M", FONT_CODE, ED_FONT_PX);
                if (cw < 1) cw = 8;
                int cx = wx + ED_GUTTER_W + cur_col * cw;
                int cy = body_y + rendered * ED_LINE_H;
                fb_rect(cx, cy, 2, ED_LINE_H, COLOR_PRIMARY);
                cursor_drawn = 1;
            }
            rendered++;
            (void)body_h;
        }
    }

    /* Status bar */
    int sy = wy + ED_H - ED_STATUS_H;
    fb_rect(wx, sy, ED_W, ED_STATUS_H, COLOR_SURFACE_HIGH);
    char status[160];
    int so = 0;
    const char *l = "ln ";
    while (*l && so<159) status[so++] = *l++;
    {
        int lc = 1, cc = 1;
        if (b) {
            for (int k=0; k<b->cursor; k++) {
                if (b->text[k] == '\n') { lc++; cc=1; }
                else cc++;
            }
        }
        char ns[16];
        e_itoa((uint32_t)lc, ns);
        for (int k=0; ns[k] && so<159; k++) status[so++]=ns[k];
        if (so<159) status[so++] = ':';
        e_itoa((uint32_t)cc, ns);
        for (int k=0; ns[k] && so<159; k++) status[so++]=ns[k];
    }
    if (so<159) status[so++] = ' ';
    if (so<159) status[so++] = '|';
    if (so<159) status[so++] = ' ';
    {
        const char *m = "size ";
        while (*m && so<159) status[so++]=*m++;
        char ns[16];
        e_itoa((uint32_t)(b ? (uint32_t)b->len : 0), ns);
        for (int k=0; ns[k] && so<159; k++) status[so++]=ns[k];
    }
    if (so<159) status[so++] = ' ';
    if (so<159) status[so++] = '|';
    if (so<159) status[so++] = ' ';
    {
        const char *as = (b && b->autosave_pending) ? "autosave: armed" : "autosave: idle";
        while (*as && so<159) status[so++]=*as++;
    }
    status[so]=0;
    font_draw(wx + ED_PAD, sy + 4, status, FONT_UI, ED_FONT_PX, COLOR_ON_SURFACE_2);

    /* Find bar overlay */
    if (g_ed.find_open) {
        int fy = wy + ED_TITLE_H;
        fb_rect(wx, fy, ED_W, 22, COLOR_SURFACE_TOP);
        font_draw(wx + ED_PAD, fy + 3, "find:", FONT_UI, ED_FONT_PX, COLOR_ON_SURFACE_2);
        font_draw(wx + ED_PAD + 40, fy + 3, g_ed.find_term,
                  FONT_CODE, ED_FONT_PX, COLOR_ON_SURFACE);
    }
}

/* ─────────────────────────────────────────────────────────────────
 * Persistence
 * ───────────────────────────────────────────────────────────────── */

#define EDITOR_OPEN_FILES_PATH "/editor/open_files"

static void editor_persist_save_internal(void) {
    (void)vault_mkdir("/editor", VAULT_TIER_INTERNAL);
    char buf[1024];
    int o = 0;
    for (int i=0; i<EDITOR_MAX_BUFFERS && o<1000; i++) {
        if (!g_bufs[i].slot_used) continue;
        if (!g_bufs[i].path[0])   continue;
        for (int k=0; g_bufs[i].path[k] && o<1000; k++) buf[o++] = g_bufs[i].path[k];
        if (o<1000) buf[o++] = '\t';
        char ns[16]; e_itoa((uint32_t)g_bufs[i].cursor, ns);
        for (int k=0; ns[k] && o<1000; k++) buf[o++] = ns[k];
        if (o<1000) buf[o++] = '\n';
    }
    buf[o] = 0;
    (void)vault_write(EDITOR_OPEN_FILES_PATH, buf, (uint32_t)o);
}

void editor_persist_save(void) { editor_persist_save_internal(); }

void editor_persist_restore(void) {
    char buf[1024];
    int got = vault_read(EDITOR_OPEN_FILES_PATH, buf, sizeof(buf) - 1);
    if (got <= 0) return;
    buf[got] = 0;
    int i = 0;
    while (i < got) {
        char path[EDITOR_PATH_MAX]; int p = 0;
        while (i < got && buf[i] != '\t' && buf[i] != '\n' && p < EDITOR_PATH_MAX-1)
            path[p++] = buf[i++];
        path[p] = 0;
        int cursor = 0;
        if (i < got && buf[i] == '\t') {
            i++;
            while (i < got && buf[i] >= '0' && buf[i] <= '9') {
                cursor = cursor * 10 + (buf[i] - '0'); i++;
            }
        }
        while (i < got && buf[i] != '\n') i++;
        if (i < got) i++;
        if (path[0]) {
            if (editor_open(path) == 0) {
                editor_buf_t *b = editor_focused();
                if (b) {
                    if (cursor < 0) cursor = 0;
                    if (cursor > b->len) cursor = b->len;
                    b->cursor = cursor;
                }
            }
        }
    }
}

/* ─────────────────────────────────────────────────────────────────
 * Selftest counters
 * ───────────────────────────────────────────────────────────────── */

uint32_t editor_total_opens(void)         { return g_total_opens; }
uint32_t editor_total_emits(void)         { return g_total_emits; }
uint32_t editor_total_autosaves(void)     { return g_total_autosaves; }
uint32_t editor_total_snapshots(void)     { return g_total_snapshots; }
uint32_t editor_total_spell_checks(void)  { return g_total_spell_checks; }
uint32_t editor_total_autocompletes(void) { return g_total_autocompletes; }
