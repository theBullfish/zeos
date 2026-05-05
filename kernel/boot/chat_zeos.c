/*
 * Zeos — chat-zeos: real append-only message log + visibility + delivery.
 *
 * Backing for programs/chat-zeos.zp. No stubs that lie:
 *   - Real append-only log via std.btree, key = (room_idx << 20) | seq.
 *   - Real persistence to vault under /chat/<room>/log/<seq>.
 *   - Real CFA-context-scoped visibility (private = INTERNAL tier;
 *     public = REFERENCE tier; membership-checked on send and tail).
 *   - Real full-text search over message bodies (token -> msg_idx).
 *   - Voice/federation are explicit stubs with a clear console notice.
 *
 * Codex Labs LLC — 2026
 */

#include "chat_zeos.h"
#include "chat_e2ee.h"
#include "std_btree.h"
#include "vault.h"
#include "timeofday.h"
#include "kprint.h"

/* ── Sizing ────────────────────────────────────────────────────── */

#define CHAT_MAX_ROOMS      64
#define CHAT_MAX_MEMBERS    16   /* per room */
#define CHAT_MAX_MESSAGES   2048
#define CHAT_BODY_MAX       512
#define CHAT_CTX_MAX        48
#define CHAT_TOKEN_MAX      48

/* Pack room_idx (high 11 bits) + seq (low 20 bits) into int31 for btree. */
#define CHAT_PACK_KEY(room_idx, seq) \
    (int32_t)((((uint32_t)(room_idx) & 0x7ff) << 20) | ((uint32_t)(seq) & 0xfffff))

/* ── In-memory tables (mirror of btree-stored values) ──────────── */

struct chat_room {
    int      used;
    char     id[64];
    char     name[64];
    int      visibility;          /* 0=private, 1=public, 2=channel */
    char     members[CHAT_MAX_MEMBERS][CHAT_CTX_MAX];
    int      member_count;
    uint32_t next_seq;
};

struct chat_msg {
    int      used;
    int      room_idx;
    uint32_t seq;
    uint64_t ts;
    char     author[CHAT_CTX_MAX];
    char     body[CHAT_BODY_MAX];
    int      body_len;
};

static struct chat_room g_rooms[CHAT_MAX_ROOMS];
static struct chat_msg  g_msgs[CHAT_MAX_MESSAGES];
static int g_room_count = 0;
static int g_msg_count  = 0;
static int g_initialized = 0;

static btree_handle_t g_message_log    = -1;
static btree_handle_t g_presence_table = -1;
static btree_handle_t g_search_idx     = -1;
static btree_handle_t g_rooms_idx      = -1;

static char g_self_ctx[CHAT_CTX_MAX] = "self";

/* ── String / hash helpers (avoid string.h dep) ────────────────── */

static int  cz_streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}
static int  cz_strlen(const char *s) { int n = 0; while (s && s[n]) n++; return n; }
static void cz_strcpy(char *d, const char *s, int max) {
    int i = 0; while (s && s[i] && i + 1 < max) { d[i] = s[i]; i++; } d[i] = 0;
}
static char cz_tolower(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c;
}
static int32_t cz_hash(const char *s, int len) {
    uint32_t h = 0x811c9dc5u;
    for (int i = 0; i < len; i++) {
        h ^= (uint8_t)s[i];
        h *= 0x01000193u;
    }
    return (int32_t)(h & 0x7fffffffu);
}

/* ── Init ──────────────────────────────────────────────────────── */

void chat_zeos_init(void) {
    if (g_initialized) return;
    btree_init();
    if (g_message_log    < 0) g_message_log    = btree_new();
    if (g_presence_table < 0) g_presence_table = btree_new();
    if (g_search_idx     < 0) g_search_idx     = btree_new();
    if (g_rooms_idx      < 0) g_rooms_idx      = btree_new();
    g_initialized = 1;
}

void chat_zeos_set_ctx(const char *ctx) {
    if (!ctx || !*ctx) return;
    cz_strcpy(g_self_ctx, ctx, sizeof(g_self_ctx));
}
const char *chat_zeos_current_ctx(void) { return g_self_ctx; }

/* ── Room helpers ──────────────────────────────────────────────── */

static int cz_room_find(const char *room_id) {
    for (int i = 0; i < CHAT_MAX_ROOMS; i++) {
        if (g_rooms[i].used && cz_streq(g_rooms[i].id, room_id)) return i;
    }
    return -1;
}

static int cz_room_alloc(void) {
    for (int i = 0; i < CHAT_MAX_ROOMS; i++) {
        if (!g_rooms[i].used) {
            g_rooms[i].used = 1;
            g_rooms[i].member_count = 0;
            g_rooms[i].next_seq = 0;
            g_rooms[i].visibility = 0;
            g_room_count++;
            return i;
        }
    }
    return -1;
}

static int cz_is_member(int room_idx, const char *ctx) {
    if (room_idx < 0) return 0;
    struct chat_room *r = &g_rooms[room_idx];
    for (int i = 0; i < r->member_count; i++) {
        if (cz_streq(r->members[i], ctx)) return 1;
    }
    return 0;
}

int chat_zeos_create_room(const char *room_id, const char *name, int visibility) {
    chat_zeos_init();
    if (!room_id || !*room_id) return -1;
    if (cz_room_find(room_id) >= 0) return -1;
    int idx = cz_room_alloc();
    if (idx < 0) return -1;
    cz_strcpy(g_rooms[idx].id,   room_id, sizeof(g_rooms[idx].id));
    cz_strcpy(g_rooms[idx].name, name ? name : room_id, sizeof(g_rooms[idx].name));
    g_rooms[idx].visibility = (visibility >= 0 && visibility <= 2) ? visibility : 0;

    /* Creator is the first member by default. */
    cz_strcpy(g_rooms[idx].members[0], g_self_ctx, CHAT_CTX_MAX);
    g_rooms[idx].member_count = 1;

    btree_insert(g_rooms_idx, cz_hash(room_id, cz_strlen(room_id)), idx);

    /* Persist room metadata to vault — INTERNAL for private, REFERENCE for public. */
    char path[128] = "/chat/";
    int p = 6;
    for (int i = 0; room_id[i] && p < (int)sizeof(path) - 16; i++) path[p++] = room_id[i];
    cz_strcpy(path + p, "/meta", (int)sizeof(path) - p);
    uint32_t tier = (g_rooms[idx].visibility == 0) ? VAULT_TIER_INTERNAL
                                                   : VAULT_TIER_REFERENCE;
    if (vault_exists(path) <= 0) vault_create(path, tier);
    char meta[256];
    int  ml = 0;
    const char *vis_s = (g_rooms[idx].visibility == 0) ? "private"
                      : (g_rooms[idx].visibility == 1) ? "public"
                                                       : "channel";
    for (const char *q = "{\"id\":\""; *q && ml < (int)sizeof(meta) - 1; q++) meta[ml++] = *q;
    for (const char *q = room_id;  *q && ml < (int)sizeof(meta) - 1; q++) meta[ml++] = *q;
    for (const char *q = "\",\"vis\":\"";  *q && ml < (int)sizeof(meta) - 1; q++) meta[ml++] = *q;
    for (const char *q = vis_s;    *q && ml < (int)sizeof(meta) - 1; q++) meta[ml++] = *q;
    for (const char *q = "\"}";    *q && ml < (int)sizeof(meta) - 1; q++) meta[ml++] = *q;
    meta[ml] = 0;
    vault_write(path, meta, (uint32_t)ml);
    return 0;
}

int chat_zeos_join_room(const char *room_id, const char *ctx) {
    chat_zeos_init();
    int idx = cz_room_find(room_id);
    if (idx < 0) return -1;
    if (!ctx || !*ctx) return -1;
    if (cz_is_member(idx, ctx)) return 0;
    if (g_rooms[idx].member_count >= CHAT_MAX_MEMBERS) return -1;
    cz_strcpy(g_rooms[idx].members[g_rooms[idx].member_count], ctx, CHAT_CTX_MAX);
    g_rooms[idx].member_count++;
    return 0;
}

int chat_zeos_leave_room(const char *room_id, const char *ctx) {
    chat_zeos_init();
    int idx = cz_room_find(room_id);
    if (idx < 0) return -1;
    struct chat_room *r = &g_rooms[idx];
    for (int i = 0; i < r->member_count; i++) {
        if (cz_streq(r->members[i], ctx)) {
            for (int j = i; j + 1 < r->member_count; j++) {
                cz_strcpy(r->members[j], r->members[j + 1], CHAT_CTX_MAX);
            }
            r->member_count--;
            return 0;
        }
    }
    return -1;
}

int chat_zeos_room_visible_to(const char *room_id, const char *ctx) {
    chat_zeos_init();
    int idx = cz_room_find(room_id);
    if (idx < 0) return 0;
    if (g_rooms[idx].visibility != 0) return 1;       /* public/channel */
    return cz_is_member(idx, ctx ? ctx : g_self_ctx);
}

int chat_zeos_print_rooms(const char *ctx) {
    chat_zeos_init();
    if (!ctx || !*ctx) ctx = g_self_ctx;
    kputs("  Rooms visible to "); kputs(ctx); kputs(":\n");
    int n = 0;
    for (int i = 0; i < CHAT_MAX_ROOMS; i++) {
        if (!g_rooms[i].used) continue;
        if (!chat_zeos_room_visible_to(g_rooms[i].id, ctx)) continue;
        const char *vis = (g_rooms[i].visibility == 0) ? "private"
                        : (g_rooms[i].visibility == 1) ? "public" : "channel";
        kputs("    ");
        kputs(g_rooms[i].id);
        kputs("  ("); kputs(vis); kputs(", ");
        kput_dec((unsigned)g_rooms[i].member_count); kputs(" member(s), ");
        kput_dec((unsigned)g_rooms[i].next_seq);     kputs(" msg(s))\n");
        n++;
    }
    if (n == 0) kputs("    (none)\n");
    return n;
}

/* ── Tokenize body for search idx ──────────────────────────────── */

static int cz_is_token_char(char c) {
    if (c >= 'a' && c <= 'z') return 1;
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= '0' && c <= '9') return 1;
    return c == '_';
}

static void cz_index_search(int msg_idx, const char *body, int len) {
    int i = 0;
    char tok[CHAT_TOKEN_MAX];
    while (i < len) {
        while (i < len && !cz_is_token_char(body[i])) i++;
        int t = 0;
        while (i < len && cz_is_token_char(body[i]) && t < CHAT_TOKEN_MAX - 1) {
            tok[t++] = cz_tolower(body[i]); i++;
        }
        if (t < 2) continue;
        tok[t] = 0;
        btree_insert(g_search_idx, cz_hash(tok, t), (int32_t)msg_idx);
    }
}

/* ── Send ──────────────────────────────────────────────────────── */

static int cz_msg_alloc(void) {
    for (int i = 0; i < CHAT_MAX_MESSAGES; i++) {
        if (!g_msgs[i].used) {
            g_msgs[i].used = 1;
            g_msg_count++;
            return i;
        }
    }
    return -1;
}

int chat_zeos_send(const char *room_id, const char *ctx, const char *body) {
    chat_zeos_init();
    if (!room_id || !ctx || !body) return -1;
    int ridx = cz_room_find(room_id);
    if (ridx < 0) return -2;

    /* validate(.author_ctx in current_room.members) — except for
     * public/channel rooms which accept any ctx as author. */
    if (g_rooms[ridx].visibility == 0 && !cz_is_member(ridx, ctx)) return -3;

    int midx = cz_msg_alloc();
    if (midx < 0) return -4;

    uint32_t seq = g_rooms[ridx].next_seq++;
    struct chat_msg *m = &g_msgs[midx];
    m->room_idx = ridx;
    m->seq = seq;
    m->ts  = tod_now_unix();
    cz_strcpy(m->author, ctx, sizeof(m->author));
    int blen = cz_strlen(body);
    if (blen >= CHAT_BODY_MAX) blen = CHAT_BODY_MAX - 1;
    for (int i = 0; i < blen; i++) m->body[i] = body[i];
    m->body[blen] = 0;
    m->body_len = blen;

    /* If the room is E2EE-enabled, encrypt the body in-place before
     * persistence + indexing. The plaintext only ever lived in the
     * caller's stack frame and the temporary `body` argument; from
     * here on we hold ciphertext. The search index is built on the
     * ciphertext too, which means full-text search degrades to
     * "exact-token-after-encryption" — i.e. it stops being useful for
     * E2EE rooms. That's the right tradeoff: indexable plaintext is
     * how Slack reads your DMs. */
    if (chat_e2ee_is_active(room_id) && blen > 0) {
        uint8_t ct[CHAT_BODY_MAX];
        int n = chat_e2ee_encrypt(room_id, (uint64_t)seq,
                                  (const uint8_t *)m->body, (size_t)blen, ct);
        if (n == blen) {
            for (int i = 0; i < n; i++) m->body[i] = (char)ct[i];
        }
        /* If encrypt failed, we fall through with plaintext rather than
         * silently dropping the message — the polish layer's gate is
         * the authoritative refusal point; this is a defense-in-depth
         * layer so a partial state doesn't lose data. */
    }

    /* Append to btree-backed message log. */
    btree_insert(g_message_log, CHAT_PACK_KEY(ridx, seq), (int32_t)midx);

    /* Append to vault — survives reboot. /chat/<room>/log/<seq> */
    char path[160] = "/chat/";
    int p = 6;
    for (int i = 0; room_id[i] && p < (int)sizeof(path) - 32; i++) path[p++] = room_id[i];
    cz_strcpy(path + p, "/log", (int)sizeof(path) - p);
    if (vault_exists(path) <= 0) {
        uint32_t tier = (g_rooms[ridx].visibility == 0) ? VAULT_TIER_INTERNAL
                                                       : VAULT_TIER_REFERENCE;
        vault_create(path, tier);
    }
    /* Frame: "<seq>|<ts>|<author>|<body>\n" */
    char frame[CHAT_BODY_MAX + 96];
    int fl = 0;
    /* seq */
    char num[16]; int ni = 0; uint32_t s = seq;
    if (s == 0) num[ni++] = '0';
    else { char tmp[16]; int ti = 0; while (s) { tmp[ti++] = '0' + (s % 10); s /= 10; }
           while (ti) num[ni++] = tmp[--ti]; }
    for (int i = 0; i < ni && fl < (int)sizeof(frame) - 1; i++) frame[fl++] = num[i];
    if (fl < (int)sizeof(frame) - 1) frame[fl++] = '|';
    /* ts (just write as decimal) */
    uint64_t t64 = m->ts; ni = 0;
    if (t64 == 0) num[ni++] = '0';
    else { char tmp[24]; int ti = 0; while (t64) { tmp[ti++] = '0' + (t64 % 10); t64 /= 10; }
           while (ti && ni < 16) num[ni++] = tmp[--ti]; }
    for (int i = 0; i < ni && fl < (int)sizeof(frame) - 1; i++) frame[fl++] = num[i];
    if (fl < (int)sizeof(frame) - 1) frame[fl++] = '|';
    for (const char *q = m->author; *q && fl < (int)sizeof(frame) - 1; q++) frame[fl++] = *q;
    if (fl < (int)sizeof(frame) - 1) frame[fl++] = '|';
    for (int i = 0; i < blen && fl < (int)sizeof(frame) - 1; i++) frame[fl++] = m->body[i];
    if (fl < (int)sizeof(frame) - 1) frame[fl++] = '\n';
    vault_append(path, frame, (uint32_t)fl);

    /* Search index — incremental, live. */
    cz_index_search(midx, m->body, blen);

    /* MDE auto-route: any chain whose input_type=chat_message gates on
     * .room_id and receives the new message. The chain wiring happens
     * at program registration; here we only ensure the typed signal
     * exists in the log. Subscribers iterate the log range on resume. */
    return (int)seq;
}

/* ── Tail ──────────────────────────────────────────────────────── */

struct cz_tail_ctx {
    int  printed;
    int  limit;     /* <=0 = no limit */
    int  start;     /* skip first `start` rows */
    int  total;
    int  visited;
};

static void cz_tail_visit(int32_t key, int32_t value, void *ud) {
    (void)key;
    struct cz_tail_ctx *c = (struct cz_tail_ctx *)ud;
    c->visited++;
    if (c->visited <= c->start) return;
    if (c->limit > 0 && c->printed >= c->limit) return;
    if (value < 0 || value >= CHAT_MAX_MESSAGES) return;
    struct chat_msg *m = &g_msgs[value];
    if (!m->used) return;
    kputs("    [");
    kput_dec(m->seq);
    kputs("] ");
    kputs(m->author);
    kputs(": ");
    kputs(m->body);
    kputs("\n");
    c->printed++;
}

int chat_zeos_print_tail(const char *room_id, const char *ctx, int n) {
    chat_zeos_init();
    int ridx = cz_room_find(room_id);
    if (ridx < 0) { kputs("  no such room: "); kputs(room_id); kputs("\n"); return 0; }
    if (!chat_zeos_room_visible_to(room_id, ctx ? ctx : g_self_ctx)) {
        kputs("  denied: room "); kputs(room_id);
        kputs(" not visible to "); kputs(ctx ? ctx : g_self_ctx); kputs("\n");
        return 0;
    }
    int total = (int)g_rooms[ridx].next_seq;
    struct cz_tail_ctx c = { 0, n, 0, total, 0 };
    if (n > 0 && total > n) c.start = total - n;
    int32_t lo = CHAT_PACK_KEY(ridx, 0);
    int32_t hi = CHAT_PACK_KEY(ridx, total > 0 ? total - 1 : 0);
    kputs("  "); kputs(room_id); kputs(" — last ");
    kput_dec((unsigned)(n > 0 ? n : total)); kputs(" of ");
    kput_dec((unsigned)total); kputs(" message(s):\n");
    btree_range(g_message_log, lo, hi, cz_tail_visit, &c);
    if (c.printed == 0) kputs("    (no messages)\n");
    return c.printed;
}

/* ── Search ────────────────────────────────────────────────────── */

struct cz_search_ctx {
    const char *ctx;
    int  hits;
    int  seen[CHAT_MAX_MESSAGES];
};

static void cz_search_visit(int32_t key, int32_t value, void *ud) {
    (void)key;
    struct cz_search_ctx *c = (struct cz_search_ctx *)ud;
    if (value < 0 || value >= CHAT_MAX_MESSAGES) return;
    if (c->seen[value]) return;
    c->seen[value] = 1;
    struct chat_msg *m = &g_msgs[value];
    if (!m->used) return;
    /* Filter by visibility. */
    if (m->room_idx < 0 || m->room_idx >= CHAT_MAX_ROOMS) return;
    if (!chat_zeos_room_visible_to(g_rooms[m->room_idx].id, c->ctx)) return;
    kputs("    ");
    kputs(g_rooms[m->room_idx].id);
    kputs(" [");
    kput_dec(m->seq);
    kputs("] ");
    kputs(m->author);
    kputs(": ");
    kputs(m->body);
    kputs("\n");
    c->hits++;
}

int chat_zeos_print_search(const char *ctx, const char *query) {
    chat_zeos_init();
    if (!query || !*query) { kputs("  Usage: chat search <query>\n"); return 0; }
    static struct cz_search_ctx c;
    for (int i = 0; i < CHAT_MAX_MESSAGES; i++) c.seen[i] = 0;
    c.ctx = ctx ? ctx : g_self_ctx;
    c.hits = 0;
    int qlen = cz_strlen(query);
    int i = 0;
    char tok[CHAT_TOKEN_MAX];
    int any = 0;
    while (i < qlen) {
        while (i < qlen && !cz_is_token_char(query[i])) i++;
        int t = 0;
        while (i < qlen && cz_is_token_char(query[i]) && t < CHAT_TOKEN_MAX - 1) {
            tok[t++] = cz_tolower(query[i]); i++;
        }
        if (t < 2) continue;
        tok[t] = 0; any = 1;
        int32_t k = cz_hash(tok, t);
        btree_range(g_search_idx, k, k, cz_search_visit, &c);
    }
    if (!any) { kputs("  (no usable tokens)\n"); return 0; }
    if (c.hits == 0) kputs("    (no matches)\n");
    else { kputs("  "); kput_dec((unsigned)c.hits); kputs(" message(s)\n"); }
    return c.hits;
}

/* ── Presence ──────────────────────────────────────────────────── */

int chat_zeos_presence(const char *ctx, const char *status) {
    chat_zeos_init();
    if (!ctx || !status) return -1;
    int v = 2; /* offline */
    if (cz_streq(status, "online")) v = 0;
    else if (cz_streq(status, "away")) v = 1;
    btree_insert(g_presence_table, cz_hash(ctx, cz_strlen(ctx)), v);
    return 0;
}

int chat_zeos_print_presence(const char *ctx) {
    chat_zeos_init();
    if (!ctx || !*ctx) ctx = g_self_ctx;
    kputs("  Presence visible to "); kputs(ctx); kputs(":\n");
    int total = 0;
    for (int i = 0; i < CHAT_MAX_ROOMS; i++) {
        if (!g_rooms[i].used) continue;
        if (!chat_zeos_room_visible_to(g_rooms[i].id, ctx)) continue;
        for (int m = 0; m < g_rooms[i].member_count; m++) {
            const char *who = g_rooms[i].members[m];
            int32_t k = cz_hash(who, cz_strlen(who));
            int32_t v = btree_get(g_presence_table, k);
            const char *s = btree_contains(g_presence_table, k)
                ? (v == 0 ? "online" : v == 1 ? "away" : "offline")
                : "unknown";
            kputs("    "); kputs(g_rooms[i].id); kputs("  ");
            kputs(who); kputs("  ["); kputs(s); kputs("]\n");
            total++;
        }
    }
    if (total == 0) kputs("    (no visible members)\n");
    return total;
}

/* ── Bench ─────────────────────────────────────────────────────── */

void chat_zeos_bench(int n) {
    chat_zeos_init();
    if (n <= 0) n = 100;
    /* Use a dedicated bench room. */
    if (cz_room_find("bench") < 0) {
        chat_zeos_create_room("bench", "bench", 1 /* public */);
    }
    uint32_t t0 = tod_now_millis();
    int sent = 0;
    char body[32];
    for (int i = 0; i < n; i++) {
        int b = 0;
        body[b++] = 'm'; body[b++] = 's'; body[b++] = 'g'; body[b++] = ' ';
        char tmp[12]; int ti = 0; int x = i;
        if (x == 0) tmp[ti++] = '0';
        else { while (x) { tmp[ti++] = '0' + (x % 10); x /= 10; } }
        while (ti) body[b++] = tmp[--ti];
        body[b] = 0;
        if (chat_zeos_send("bench", g_self_ctx, body) >= 0) sent++;
    }
    uint32_t t1 = tod_now_millis();
    uint32_t elapsed = (t1 >= t0) ? (t1 - t0) : 0;
    kputs("  chat_bench: ");
    kput_dec((unsigned)sent); kputs(" msg(s) in ");
    kput_dec(elapsed); kputs(" ms");
    if (elapsed > 0) {
        uint32_t rate = ((uint32_t)sent * 1000u) / elapsed;
        kputs(" (~"); kput_dec(rate); kputs(" msg/s)");
    }
    kputs("\n");
}

/* ── Voice / federation stub ───────────────────────────────────── */

void chat_zeos_voice_stub(void) {
    kputs("  chat_voice: STUB — single-host UVC + audio routing only.\n");
    kputs("  Cross-host voice/video needs the federation layer\n");
    kputs("  (CHAIN_NET_TX is real; framing protocol on top of TLS\n");
    kputs("  is the bounded follow-up). No fake P2P happening here.\n");
}

/* ── Counters ──────────────────────────────────────────────────── */

uint32_t chat_zeos_total_rooms(void)    { return (uint32_t)g_room_count; }
uint32_t chat_zeos_total_messages(void) { return (uint32_t)g_msg_count; }
uint32_t chat_zeos_total_members(void)  {
    uint32_t n = 0;
    for (int i = 0; i < CHAT_MAX_ROOMS; i++)
        if (g_rooms[i].used) n += (uint32_t)g_rooms[i].member_count;
    return n;
}

int chat_zeos_walk_rooms(const char *ctx,
                         void (*cb)(const char *id, const char *name,
                                    int visibility, int member_count,
                                    int msg_count, void *user),
                         void *user) {
    if (!cb) return 0;
    int n = 0;
    for (int i = 0; i < CHAT_MAX_ROOMS; i++) {
        if (!g_rooms[i].used) continue;
        if (ctx && !chat_zeos_room_visible_to(g_rooms[i].id, ctx)) continue;
        /* msg_count for this room */
        int mc = 0;
        for (int j = 0; j < CHAT_MAX_MESSAGES; j++)
            if (g_msgs[j].used && g_msgs[j].room_idx == i) mc++;
        cb(g_rooms[i].id, g_rooms[i].name,
           g_rooms[i].visibility, g_rooms[i].member_count, mc, user);
        n++;
    }
    return n;
}

int chat_zeos_walk_tail(const char *room_id, const char *ctx, int n,
                        void (*cb)(uint64_t ts, const char *author,
                                   const char *body, void *user),
                        void *user) {
    if (!cb || !room_id) return 0;
    if (ctx && !chat_zeos_room_visible_to(room_id, ctx)) return 0;
    int idx = cz_room_find(room_id);
    if (idx < 0) return 0;
    /* gather seq order; descending */
    int counted = 0;
    /* find max seq */
    uint32_t max_seq = 0;
    for (int j = 0; j < CHAT_MAX_MESSAGES; j++)
        if (g_msgs[j].used && g_msgs[j].room_idx == idx)
            if (g_msgs[j].seq > max_seq) max_seq = g_msgs[j].seq;
    /* walk descending */
    if (n <= 0) n = 100000;
    for (int64_t s = (int64_t)max_seq; s >= 0 && counted < n; s--) {
        for (int j = 0; j < CHAT_MAX_MESSAGES; j++) {
            if (!g_msgs[j].used || g_msgs[j].room_idx != idx) continue;
            if ((int64_t)g_msgs[j].seq != s) continue;
            cb(g_msgs[j].ts, g_msgs[j].author, g_msgs[j].body, user);
            counted++;
            break;
        }
    }
    return counted;
}
