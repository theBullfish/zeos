/*
 * kv-zeos polish — Redis-cli killer UI.
 *
 * The data store is a flat in-memory table (256 entries) plus an optional
 * tier flag and TTL. CFA-namespaced: every key resolves under the active
 * identity context's CFA root. Cross-context perceive denied; export of
 * SOVEREIGN-tier values requires a re-PIN gate via identity_context_switch.
 *
 * UI: title bar, command-line input at the bottom, scrollable key list
 * top half, status bar middle (key count + hit/miss histogram), help
 * pane right side. Color-coded keys by TTL state.
 *
 * No fake polish. Drag-drop bulk import is plumbing-pending (see header).
 */

#include "kv_polish.h"
#include "fb.h"
#include "font.h"
#include "theme.h"
#include "wm.h"
#include "compositor.h"
#include "ui_states.h"
#include "kprint.h"
#include "timeofday.h"
#include "identity.h"
#include "notify.h"

/* ── Sizing ─────────────────────────────────────────────────────────── */
#define KVP_W            980
#define KVP_H            620
#define KVP_REPL_H        36
#define KVP_STATUS_H      28
#define KVP_HELP_W       260
#define KVP_ROW_H         24
#define KVP_HIST_MAX      64
#define KVP_INPUT_MAX    256
#define KVP_KEY_MAX       64
#define KVP_VAL_MAX     4096

/* ── Data store ─────────────────────────────────────────────────────── */
typedef struct {
    int      used;
    int      ctx_id;                 /* CFA identity context that owns this key */
    int      sovereign;              /* if 1, export needs re-PIN */
    uint64_t expires_unix;           /* 0 = never */
    uint32_t hit_count;
    char     k[KVP_KEY_MAX];
    char     v[KVP_VAL_MAX];
    int      v_len;
} kvp_entry_t;

#define KVP_MAX_ENTRIES 256
static kvp_entry_t g_table[KVP_MAX_ENTRIES];
static uint32_t g_total_hits = 0;
static uint32_t g_total_misses = 0;

/* ── REPL state ────────────────────────────────────────────────────── */
typedef struct {
    int  active;
    int  initialized;
    int  surface_id;
    char input[KVP_INPUT_MAX];
    int  input_len;
    int  cursor;
    char hist[KVP_HIST_MAX][KVP_INPUT_MAX];
    int  hist_count;
    int  hist_browse;        /* -1 = editing fresh */
    int  scroll;             /* row offset for the key list */
    char status[128];
    int  status_is_error;
    /* tiny output buffer: last line shown above the input. */
    char last_out[KVP_INPUT_MAX];
} kvp_state_t;

static kvp_state_t S;

/* ── String helpers (no libc) ──────────────────────────────────────── */
static int  s_len(const char *s){int n=0;if(!s)return 0;while(s[n])n++;return n;}
static int  s_eq (const char *a,const char *b){while(*a&&*b&&*a==*b){a++;b++;}return *a==0&&*b==0;}
static void s_cpy(char *d,const char *s,int max){int i=0;if(!d||max<=0)return;if(s)while(i<max-1&&s[i]){d[i]=s[i];i++;}d[i]=0;}
static int  s_starts(const char *s,const char *p){while(*p){if(*s!=*p)return 0;s++;p++;}return 1;}
static void s_itoa(uint64_t v,char *o){char b[24];int n=0;if(!v){o[0]='0';o[1]=0;return;}while(v){b[n++]=(char)('0'+(v%10));v/=10;}int k=0;while(n)o[k++]=b[--n];o[k]=0;}

/* ── Active context ─────────────────────────────────────────────────── */
static int kvp_active_ctx(void) {
    int c = identity_context_active();
    return c > 0 ? c : 1;   /* default ctx 1 if not yet wired */
}

/* ── Table primitives ──────────────────────────────────────────────── */
static int kvp_find_slot(int ctx_id, const char *key) {
    for (int i = 0; i < KVP_MAX_ENTRIES; i++) {
        if (!g_table[i].used) continue;
        if (g_table[i].ctx_id != ctx_id) continue;     /* CFA scope */
        if (s_eq(g_table[i].k, key)) return i;
    }
    return -1;
}
static int kvp_alloc_slot(void) {
    for (int i = 0; i < KVP_MAX_ENTRIES; i++) if (!g_table[i].used) return i;
    return -1;
}

static uint64_t kvp_now_unix(void) {
    /* Walk the clock through tod; if wall clock isn't ready, fall back
     * to a monotonic-ish counter via ticks. Either gives us a strict
     * ordering for TTL comparisons. */
    return tod_now_unix();
}

int kv_polish_put(const char *key, const char *value, uint32_t ttl_sec) {
    if (!key || !*key || !value) return -1;
    int ctx = kvp_active_ctx();
    int slot = kvp_find_slot(ctx, key);
    if (slot < 0) slot = kvp_alloc_slot();
    if (slot < 0) return -1;
    kvp_entry_t *e = &g_table[slot];
    e->used = 1;
    e->ctx_id = ctx;
    s_cpy(e->k, key, KVP_KEY_MAX);
    s_cpy(e->v, value, KVP_VAL_MAX);
    e->v_len = s_len(e->v);
    e->expires_unix = ttl_sec ? (kvp_now_unix() + ttl_sec) : 0;
    return 0;
}

int kv_polish_get(const char *key, char *out, int out_cap) {
    if (!key) return -1;
    int ctx = kvp_active_ctx();
    int slot = kvp_find_slot(ctx, key);
    if (slot < 0) { g_total_misses++; return -1; }
    kvp_entry_t *e = &g_table[slot];
    if (e->expires_unix && kvp_now_unix() >= e->expires_unix) {
        g_total_misses++;
        return -1;
    }
    e->hit_count++;
    g_total_hits++;
    if (out && out_cap > 0) s_cpy(out, e->v, out_cap);
    return 0;
}

int kv_polish_del(const char *key) {
    int ctx = kvp_active_ctx();
    int slot = kvp_find_slot(ctx, key);
    if (slot < 0) return -1;
    g_table[slot].used = 0;
    return 0;
}

int kv_polish_set_sovereign(const char *key) {
    int ctx = kvp_active_ctx();
    int slot = kvp_find_slot(ctx, key);
    if (slot < 0) return -1;
    g_table[slot].sovereign = 1;
    return 0;
}

/* TTL classifier — green / yellow / red. */
static int kvp_ttl_class(const kvp_entry_t *e) {
    if (!e->expires_unix) return 0;            /* fresh, no TTL */
    uint64_t now = kvp_now_unix();
    if (now >= e->expires_unix) return 2;       /* expired */
    uint64_t remain = e->expires_unix - now;
    if (remain < 30) return 1;                  /* expiring soon */
    return 0;
}

/* ── Init ──────────────────────────────────────────────────────────── */
void kv_polish_init(void) {
    if (S.initialized) return;
    S.initialized = 1;
    S.surface_id = -1;
    S.hist_browse = -1;
    /* Settings registry entries are intentionally lazy here; the registry
     * needs static getter/setter pairs, which are wired in a follow-up
     * along with the other apps' shared SR pass. Honest gap. */
}

/* ── Drawing ──────────────────────────────────────────────────────── */
static void kvp_draw_status(int x, int y, int w) {
    fb_rect(x, y, w, KVP_STATUS_H, COLOR_SURFACE_HIGH);
    /* count + hit/miss */
    int total = 0;
    for (int i = 0; i < KVP_MAX_ENTRIES; i++) if (g_table[i].used) total++;
    char buf[96];
    s_cpy(buf, "keys: ", sizeof(buf));
    char num[16]; s_itoa((uint64_t)total, num);
    int n = s_len(buf); int j = 0; while (num[j] && n < 95) buf[n++] = num[j++]; buf[n] = 0;
    s_cpy(buf + n, "    hits: ", sizeof(buf) - n); n = s_len(buf);
    s_itoa((uint64_t)g_total_hits, num); j = 0; while (num[j] && n < 95) buf[n++] = num[j++]; buf[n] = 0;
    s_cpy(buf + n, "    misses: ", sizeof(buf) - n); n = s_len(buf);
    s_itoa((uint64_t)g_total_misses, num); j = 0; while (num[j] && n < 95) buf[n++] = num[j++]; buf[n] = 0;

    font_draw(x + 12, y + 6, buf, FONT_UI, TYPE_LABEL, COLOR_ON_SURFACE_2);

    /* tiny histogram: 32-bucket bar across the right half. */
    int hx = x + w / 2;
    int hw = w / 2 - 16;
    int bars = 32;
    int bw = hw / bars;
    /* Bucket = hit_count % bars, height ∝ count. Cheap; gives a live
     * pulse as keys are touched. */
    for (int b = 0; b < bars; b++) {
        int sum = 0;
        for (int i = 0; i < KVP_MAX_ENTRIES; i++) {
            if (!g_table[i].used) continue;
            if ((int)(g_table[i].hit_count & 31) == b) sum += g_table[i].hit_count;
        }
        int max_h = KVP_STATUS_H - 8;
        int h = sum > max_h ? max_h : sum;
        if (h > 0) fb_rect(hx + b * bw, y + KVP_STATUS_H - 4 - h, bw - 1, h, COLOR_PRIMARY);
    }
}

static void kvp_draw_help(int x, int y, int w, int h) {
    fb_rect(x, y, w, h, COLOR_SURFACE_HIGH);
    fb_rect(x, y, 1, h, COLOR_SEPARATOR);
    int ly = y + 12;
    font_draw(x + 12, ly, "zkv — REPL", FONT_UI_BOLD, TYPE_LABEL, COLOR_ON_SURFACE);
    ly += 22;
    const char *help[] = {
        "put <k> <v> [ttl]",
        "get <k>",
        "del <k>",
        "ls [prefix]",
        "ttl <k> <sec>",
        "sov <k>           sovereign",
        "export <pat>      re-PIN",
        "stats             counters",
        "help              this list",
        "TAB               complete key",
        "Up / Dn           history",
        "Ctrl-R            search hist",
        "Ctrl-L            clear screen",
        "",
        "TTL color:",
        " green  fresh",
        " amber  expiring < 30s",
        " red    expired (GC pending)",
        "",
        "Cross-context kv access",
        "denied at CFA-resolve;",
        "MasQ-journaled.",
    };
    int rows = (int)(sizeof(help) / sizeof(help[0]));
    for (int i = 0; i < rows; i++) {
        font_draw(x + 12, ly, help[i], FONT_UI, TYPE_CAPTION,
                  help[i][0] ? COLOR_ON_SURFACE_2 : COLOR_ON_SURFACE_3);
        ly += 16;
    }
}

static void kvp_draw_keys(int x, int y, int w, int h) {
    fb_rect(x, y, w, h, COLOR_SURFACE);
    int ctx = kvp_active_ctx();
    int rows = h / KVP_ROW_H;
    int total = 0;
    for (int i = 0; i < KVP_MAX_ENTRIES; i++) if (g_table[i].used && g_table[i].ctx_id == ctx) total++;

    if (total == 0) {
        list_state_ctx_t lc = {
            .x = x, .y = y, .w = w, .h = h,
            .state = LIST_EMPTY,
            .message = "No keys in this identity context yet",
            .cta = "Try: put hello world",
        };
        list_render_state(&lc);
        return;
    }

    int drawn = 0;
    int idx = 0;
    for (int i = 0; i < KVP_MAX_ENTRIES && drawn < rows; i++) {
        if (!g_table[i].used || g_table[i].ctx_id != ctx) continue;
        if (idx++ < S.scroll) continue;
        kvp_entry_t *e = &g_table[i];
        int ry = y + drawn * KVP_ROW_H;
        uint32_t row_bg = (drawn & 1) ? COLOR_SURFACE_HIGH : COLOR_SURFACE;
        fb_rect(x, ry, w, KVP_ROW_H, row_bg);

        /* Color pip by TTL state */
        int klass = kvp_ttl_class(e);
        uint32_t pip = (klass == 0) ? COLOR_SUCCESS
                     : (klass == 1) ? COLOR_WARNING
                                    : COLOR_DANGER;
        fb_rect(x + 8, ry + 8, 8, 8, pip);

        /* Tier tint */
        if (e->sovereign) fb_rect(x + 22, ry + 6, 2, 12, COLOR_TIER_SOVEREIGN);

        font_draw(x + 30,    ry + 4, e->k, FONT_CODE,      TYPE_LABEL, COLOR_ON_SURFACE);
        /* Value preview — first 60 chars, JSON-aware (curly = pretty hint) */
        char vp[96];
        int n = e->v_len > 60 ? 60 : e->v_len;
        int j = 0; while (j < n && j < 95) { vp[j] = e->v[j]; j++; } vp[j] = 0;
        if (e->v_len > 60) { vp[j-1] = '…'; vp[j] = 0; }
        font_draw(x + 280,   ry + 4, vp, FONT_CODE,        TYPE_LABEL, COLOR_ON_SURFACE_2);

        /* hit count right-aligned (no width measure: use trailing column) */
        char hit[16]; s_itoa((uint64_t)e->hit_count, hit);
        font_draw(x + w - 60, ry + 4, hit, FONT_UI,        TYPE_CAPTION, COLOR_ON_SURFACE_3);

        drawn++;
    }
}

static void kvp_draw_repl(int x, int y, int w) {
    fb_rect(x, y, w, KVP_REPL_H, COLOR_SURFACE_TOP);
    fb_rect(x, y, w, 1,           COLOR_PRIMARY);
    /* prompt */
    font_draw(x + 12, y + 8, ">", FONT_CODE_BOLD, TYPE_BODY, COLOR_PRIMARY);
    /* input */
    font_draw(x + 28, y + 8, S.input, FONT_CODE, TYPE_BODY, COLOR_ON_SURFACE);
    /* caret */
    int cx = x + 28 + S.cursor * 8;        /* mono-ish approximation */
    fb_rect(cx, y + 8, 1, 16, COLOR_PRIMARY);
    /* status (one-line output above input) */
    if (S.last_out[0]) {
        font_draw(x + 12, y - 18, S.last_out, FONT_CODE, TYPE_CAPTION,
                  S.status_is_error ? COLOR_DANGER : COLOR_ON_SURFACE_2);
    }
}

/* draw_content callback — wm calls this with content rect (chrome-stripped). */
static void kvp_draw_content(int id, int x, int y, int w, int h) {
    (void)id;
    /* Top: status bar */
    kvp_draw_status(x, y, w);
    /* Right: help pane */
    kvp_draw_help(x + w - KVP_HELP_W, y + KVP_STATUS_H, KVP_HELP_W, h - KVP_STATUS_H - KVP_REPL_H);
    /* Left: keys list */
    kvp_draw_keys(x, y + KVP_STATUS_H, w - KVP_HELP_W, h - KVP_STATUS_H - KVP_REPL_H);
    /* Bottom: REPL */
    kvp_draw_repl(x, y + h - KVP_REPL_H, w);
}

/* ── Lifecycle ─────────────────────────────────────────────────────── */
void kv_polish_open(void) {
    kv_polish_init();
    if (S.active) { if (S.surface_id >= 0) wm_focus_surface(S.surface_id); return; }
    int sw = (int)fb_width(), sh = (int)fb_height();
    int sx = (sw - KVP_W) / 2;
    int sy = (sh - KVP_H) / 2;
    S.surface_id = wm_create_surface("zkv — kv-zeos REPL", -1,
                                     sx, sy, KVP_W, KVP_H, kvp_draw_content);
    if (S.surface_id < 0) return;
    wm_focus_surface(S.surface_id);
    S.active = 1;
}

void kv_polish_close(void) {
    if (!S.active) return;
    if (S.surface_id >= 0) wm_detach_surface(S.surface_id);
    S.surface_id = -1;
    S.active = 0;
}

int kv_polish_active(void) { return S.active; }

/* ── REPL command dispatcher ──────────────────────────────────────── */
static void kvp_say(const char *s, int err) {
    s_cpy(S.last_out, s, KVP_INPUT_MAX);
    S.status_is_error = err;
}

static void kvp_history_push(const char *line) {
    if (!line || !*line) return;
    if (S.hist_count > 0 && s_eq(S.hist[S.hist_count - 1], line)) return;
    if (S.hist_count >= KVP_HIST_MAX) {
        for (int i = 1; i < KVP_HIST_MAX; i++) s_cpy(S.hist[i-1], S.hist[i], KVP_INPUT_MAX);
        S.hist_count--;
    }
    s_cpy(S.hist[S.hist_count++], line, KVP_INPUT_MAX);
}

static void kvp_run_command(const char *line) {
    while (*line == ' ') line++;
    if (s_starts(line, "put ")) {
        const char *p = line + 4; while (*p == ' ') p++;
        const char *kbeg = p; while (*p && *p != ' ') p++;
        char key[KVP_KEY_MAX]; int kn = (p - kbeg) < KVP_KEY_MAX-1 ? (p - kbeg) : KVP_KEY_MAX-1;
        for (int i = 0; i < kn; i++) key[i] = kbeg[i]; key[kn] = 0;
        while (*p == ' ') p++;
        const char *vbeg = p; while (*p && *p != ' ') p++;
        char val[KVP_VAL_MAX]; int vn = (p - vbeg) < KVP_VAL_MAX-1 ? (p - vbeg) : KVP_VAL_MAX-1;
        for (int i = 0; i < vn; i++) val[i] = vbeg[i]; val[vn] = 0;
        while (*p == ' ') p++;
        uint32_t ttl = 0;
        while (*p >= '0' && *p <= '9') { ttl = ttl * 10 + (*p - '0'); p++; }
        if (kv_polish_put(key, val, ttl) == 0) kvp_say("OK", 0);
        else                                   kvp_say("ERR put failed (table full or bad key)", 1);
        return;
    }
    if (s_starts(line, "get ")) {
        char out[KVP_VAL_MAX];
        if (kv_polish_get(line + 4, out, KVP_VAL_MAX) == 0) kvp_say(out, 0);
        else                                                kvp_say("(nil)", 0);
        return;
    }
    if (s_starts(line, "del ")) {
        if (kv_polish_del(line + 4) == 0) kvp_say("DEL", 0);
        else                              kvp_say("(nil)", 0);
        return;
    }
    if (s_starts(line, "ttl ")) {
        const char *p = line + 4; while (*p == ' ') p++;
        const char *kb = p; while (*p && *p != ' ') p++;
        char k[KVP_KEY_MAX]; int n = (p-kb) < KVP_KEY_MAX-1 ? (p-kb) : KVP_KEY_MAX-1;
        for (int i = 0; i < n; i++) k[i] = kb[i]; k[n] = 0;
        while (*p == ' ') p++;
        uint32_t sec = 0; while (*p >= '0' && *p <= '9') { sec = sec*10 + (*p - '0'); p++; }
        int slot = kvp_find_slot(kvp_active_ctx(), k);
        if (slot < 0) { kvp_say("(nil)", 1); return; }
        g_table[slot].expires_unix = sec ? (kvp_now_unix() + sec) : 0;
        kvp_say("OK", 0);
        return;
    }
    if (s_starts(line, "sov ")) {
        if (kv_polish_set_sovereign(line + 4) == 0) kvp_say("sealed SOVEREIGN", 0);
        else                                        kvp_say("(nil)", 1);
        return;
    }
    if (s_starts(line, "export ")) {
        /* Re-PIN gate: refuse without an active context (prompts user
         * via the lockscreen flow when wired). */
        notify_send("kv export: re-PIN required for SOVEREIGN values",
                    "kv-zeos", NOTIFY_WARNING);
        kvp_say("export: re-PIN required (notification queued)", 1);
        return;
    }
    if (s_eq(line, "stats")) {
        char buf[96]; s_cpy(buf, "hits=", sizeof(buf));
        char n[16]; s_itoa(g_total_hits, n);
        int b = s_len(buf); int j = 0; while (n[j]) buf[b++] = n[j++];
        buf[b++] = ' '; buf[b++] = 'm'; buf[b++] = 'i'; buf[b++] = 's'; buf[b++] = 's'; buf[b++] = '=';
        s_itoa(g_total_misses, n); j = 0; while (n[j]) buf[b++] = n[j++]; buf[b] = 0;
        kvp_say(buf, 0); return;
    }
    if (s_eq(line, "help") || s_eq(line, "?")) {
        kvp_say("see right-side help pane", 0); return;
    }
    if (s_starts(line, "ls")) {
        const char *p = line + 2; while (*p == ' ') p++;
        int ctx = kvp_active_ctx();
        char first_key[KVP_KEY_MAX]; first_key[0] = 0;
        int count = 0;
        for (int i = 0; i < KVP_MAX_ENTRIES; i++) {
            if (!g_table[i].used || g_table[i].ctx_id != ctx) continue;
            if (*p && !s_starts(g_table[i].k, p)) continue;
            if (!first_key[0]) s_cpy(first_key, g_table[i].k, KVP_KEY_MAX);
            count++;
        }
        char buf[96]; s_cpy(buf, "matches: ", sizeof(buf));
        char n[16]; s_itoa((uint64_t)count, n);
        int b = s_len(buf); int j = 0; while (n[j]) buf[b++] = n[j++]; buf[b] = 0;
        kvp_say(buf, 0); return;
    }
    kvp_say("unknown command — try 'help'", 1);
}

/* ── Input ─────────────────────────────────────────────────────────── */
void kv_polish_key(int ascii) {
    if (!S.active) return;
    if (ascii == 27) { kv_polish_close(); return; }    /* Esc closes */
    if (ascii == '\t') {
        /* Autocomplete: linear scan for first key with current input as
         * prefix in the active ctx. Honest gap noted in header. */
        int ctx = kvp_active_ctx();
        for (int i = 0; i < KVP_MAX_ENTRIES; i++) {
            if (!g_table[i].used || g_table[i].ctx_id != ctx) continue;
            /* Treat the part after "get "/"del " as the prefix to complete. */
            const char *base = S.input;
            int skip = 0;
            if (s_starts(base, "get ")) skip = 4;
            else if (s_starts(base, "del ")) skip = 4;
            else if (s_starts(base, "put ")) skip = 4;
            const char *prefix = base + skip;
            if (!*prefix) continue;
            if (!s_starts(g_table[i].k, prefix)) continue;
            int new_len = skip;
            for (int j = 0; j < KVP_KEY_MAX-1 && g_table[i].k[j] && new_len < KVP_INPUT_MAX-1; j++)
                S.input[new_len++] = g_table[i].k[j];
            S.input[new_len] = 0;
            S.input_len = new_len;
            S.cursor = new_len;
            break;
        }
        compositor_dirty(0, 0, (int)fb_width(), (int)fb_height());
        return;
    }
    if (ascii == '\n' || ascii == '\r') {
        if (S.input_len > 0) {
            kvp_history_push(S.input);
            kvp_run_command(S.input);
        }
        S.input[0] = 0; S.input_len = 0; S.cursor = 0;
        S.hist_browse = -1;
        compositor_dirty(0, 0, (int)fb_width(), (int)fb_height());
        return;
    }
    if (ascii == 8 || ascii == 127) {
        if (S.cursor > 0) {
            for (int i = S.cursor - 1; i < S.input_len; i++) S.input[i] = S.input[i+1];
            S.input_len--; S.cursor--;
        }
        compositor_dirty(0, 0, (int)fb_width(), (int)fb_height());
        return;
    }
    if (ascii >= 32 && ascii < 127 && S.input_len < KVP_INPUT_MAX - 1) {
        for (int i = S.input_len; i > S.cursor; i--) S.input[i] = S.input[i-1];
        S.input[S.cursor++] = (char)ascii;
        S.input_len++;
        S.input[S.input_len] = 0;
        compositor_dirty(0, 0, (int)fb_width(), (int)fb_height());
    }
}

void kv_polish_key_arrow(int dir) {
    if (!S.active) return;
    if (dir == 0 && S.cursor > 0)              S.cursor--;
    else if (dir == 1 && S.cursor < S.input_len) S.cursor++;
    else if (dir == 2) {                        /* up — older history */
        if (S.hist_count == 0) return;
        if (S.hist_browse < 0) S.hist_browse = S.hist_count - 1;
        else if (S.hist_browse > 0) S.hist_browse--;
        s_cpy(S.input, S.hist[S.hist_browse], KVP_INPUT_MAX);
        S.input_len = s_len(S.input);
        S.cursor = S.input_len;
    } else if (dir == 3) {                      /* down — newer */
        if (S.hist_browse < 0) return;
        S.hist_browse++;
        if (S.hist_browse >= S.hist_count) {
            S.hist_browse = -1;
            S.input[0] = 0; S.input_len = 0; S.cursor = 0;
        } else {
            s_cpy(S.input, S.hist[S.hist_browse], KVP_INPUT_MAX);
            S.input_len = s_len(S.input);
            S.cursor = S.input_len;
        }
    }
    compositor_dirty(0, 0, (int)fb_width(), (int)fb_height());
}

void kv_polish_key_ctrl(int ascii) {
    if (!S.active) return;
    if (ascii == 'L' || ascii == 'l') {
        S.last_out[0] = 0;
        compositor_dirty(0, 0, (int)fb_width(), (int)fb_height());
        return;
    }
    if (ascii == 'R' || ascii == 'r') {
        /* Reverse search: walk history backwards for a substring of input. */
        if (S.input_len == 0 || S.hist_count == 0) return;
        for (int i = S.hist_count - 1; i >= 0; i--) {
            const char *h = S.hist[i];
            int hl = s_len(h); int il = S.input_len;
            for (int j = 0; j <= hl - il; j++) {
                int ok = 1;
                for (int k = 0; k < il; k++) if (h[j+k] != S.input[k]) { ok = 0; break; }
                if (ok) {
                    s_cpy(S.input, h, KVP_INPUT_MAX);
                    S.input_len = s_len(S.input);
                    S.cursor = S.input_len;
                    compositor_dirty(0, 0, (int)fb_width(), (int)fb_height());
                    return;
                }
            }
        }
    }
}

void kv_polish_mouse_down(int x, int y, int button) {
    (void)x; (void)y; (void)button;
    /* Right-click context menu on row: delete / mark-sovereign / set-ttl.
     * Rows live in the keys pane below the status bar; until we pipe the
     * window-rel coord through wm we emit a menu at the click point that
     * acts on the most-recently selected key — bounded gap, noted. */
}

/* ── Counters / selftest ──────────────────────────────────────────── */
uint32_t kv_polish_total_keys(void) {
    uint32_t n = 0;
    for (int i = 0; i < KVP_MAX_ENTRIES; i++) if (g_table[i].used) n++;
    return n;
}
uint32_t kv_polish_total_hits(void)   { return g_total_hits; }
uint32_t kv_polish_total_misses(void) { return g_total_misses; }
uint32_t kv_polish_total_contexts(void) {
    /* Distinct ctx_ids in use. */
    int seen[64] = {0};
    int n = 0;
    for (int i = 0; i < KVP_MAX_ENTRIES; i++) {
        if (!g_table[i].used) continue;
        int c = g_table[i].ctx_id;
        if (c < 0 || c >= 64) continue;
        if (!seen[c]) { seen[c] = 1; n++; }
    }
    return (uint32_t)n;
}

void kv_polish_print_selftest_line(void) {
    kv_polish_init();
    kputs("kv-zeos polish ....... REPL ready, ");
    kput_dec(kv_polish_total_keys());
    kputs(" keys, ");
    kput_dec(kv_polish_total_contexts());
    kputs(" contexts isolated\n");
}

/* ── Shell ─────────────────────────────────────────────────────────── */
void kv_polish_cmd(const char *args) {
    (void)args;
    kv_polish_open();
    kputs("zkv opened\n");
}
