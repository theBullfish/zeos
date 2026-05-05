/*
 * Zeos -- Filesystem event chain (CHAIN_FS_EVENT) + subscribers.
 *
 * Every FS mutation in the system emits a typed fs_event. The four
 * subscriber chains (trash_react, index, undo, notify) consume those
 * events via MDE type-routing. The file manager UI registers a small
 * non-chain listener so it can repaint when ANY process mutates the
 * filesystem -- editor autosave, shell `rm`, file manager itself,
 * trash restore, future cloud sync.
 *
 * Pipeline:
 *   CHAIN_FS_EVENT:        fs_request -> validate -> execute -> emit_event
 *                          (output: fs_event)
 *   CHAIN_FS_TRASH_REACT:  fs_event -> match_delete -> log_trash_reaction
 *   CHAIN_FS_INDEX:        fs_event -> bucket_path -> update_count
 *   CHAIN_FS_UNDO:         fs_event -> derive_reverse -> push_ring
 *   CHAIN_FS_NOTIFY:       fs_event -> classify -> notify_send
 *
 * The "execute" node here is intentionally a no-op: by the time
 * fs_event_emit is called, the underlying fat32_* mutation has
 * already succeeded. The chain shape is the record. Future versions
 * may move the actual mutation INSIDE the chain by routing
 * fs_event_request -> validate -> execute(fat32_*_raw). For now the
 * chain is the emission/subscriber spine, and the fat32 imperative
 * surface stays as the bridge.
 */

#include "fs_event.h"
#include "chain.h"
#include "chain_registry.h"
#include "kprint.h"
#include "timer.h"
#include "timeofday.h"
#include "fat32.h"
#include "vault.h"
#include "notify.h"

/* ── Chain ids ──────────────────────────────────────────────────── */
int CHAIN_FS_EVENT        = -1;
int CHAIN_FS_TRASH_REACT  = -1;
int CHAIN_FS_INDEX        = -1;
int CHAIN_FS_UNDO         = -1;
int CHAIN_FS_NOTIFY       = -1;

/* ── Pending event slot (consumed by emitter pipeline) ──────────── */
static fs_request_t s_pending_req;
static int          s_pending_valid;
static fs_event_t   s_last_event;
static int          s_seq;

/* ── Tiny helpers ────────────────────────────────────────────────── */
static void fe_memset(void *d, int v, int n) {
    uint8_t *dd = (uint8_t *)d;
    for (int i = 0; i < n; i++) dd[i] = (uint8_t)v;
}
static void fe_strncpy(char *d, const char *s, int max) {
    int i = 0;
    if (!d || max <= 0) return;
    if (s) while (i < max - 1 && s[i]) { d[i] = s[i]; i++; }
    d[i] = 0;
}
static int fe_streq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static int fe_strlen(const char *s) {
    int n = 0; if (!s) return 0; while (s[n]) n++; return n;
}
/* Returns the parent dir of `path` written into out (slash-terminated
 * or "/" for root). Truncates at out_max-1. */
static void fe_parent_dir(const char *path, char *out, int out_max) {
    if (!path || !out || out_max <= 0) { if (out && out_max > 0) out[0] = 0; return; }
    int n = fe_strlen(path);
    int last_slash = -1;
    for (int i = 0; i < n; i++) if (path[i] == '/') last_slash = i;
    if (last_slash <= 0) { fe_strncpy(out, "/", out_max); return; }
    int copy = last_slash;
    if (copy >= out_max) copy = out_max - 1;
    for (int i = 0; i < copy; i++) out[i] = path[i];
    out[copy] = 0;
}

/* ── Listener (UI bridge) ───────────────────────────────────────── */
#define FS_EVENT_LISTENERS_MAX 4
static fs_event_listener_fn s_listeners[FS_EVENT_LISTENERS_MAX];
static void                *s_listener_ctx[FS_EVENT_LISTENERS_MAX];
static int                  s_listener_count;

int fs_event_register_listener(fs_event_listener_fn fn, void *ctx) {
    if (!fn) return -1;
    if (s_listener_count >= FS_EVENT_LISTENERS_MAX) return -1;
    s_listeners[s_listener_count]    = fn;
    s_listener_ctx[s_listener_count] = ctx;
    s_listener_count++;
    return 0;
}

static void fire_listeners(const fs_event_t *e) {
    for (int i = 0; i < s_listener_count; i++) {
        if (s_listeners[i]) s_listeners[i](e, s_listener_ctx[i]);
    }
}

/* ── Counters ───────────────────────────────────────────────────── */
static uint32_t g_total_emits;
static uint32_t g_total_trash_reacts;
static uint32_t g_total_index_updates;
static uint32_t g_total_undo_records;
static uint32_t g_total_notify_fires;

uint32_t fs_event_total_emits(void)         { return g_total_emits; }
uint32_t fs_event_total_trash_reacts(void)  { return g_total_trash_reacts; }
uint32_t fs_event_total_index_updates(void) { return g_total_index_updates; }
uint32_t fs_event_total_undo_records(void)  { return g_total_undo_records; }
uint32_t fs_event_total_notify_fires(void)  { return g_total_notify_fires; }

/* ─────────────────────────────────────────────────────────────────
 * Emitter pipeline (CHAIN_FS_EVENT)
 *   fs_request -> validate -> execute -> emit_event
 * ───────────────────────────────────────────────────────────────── */

static void node_fs_request(chain_node_t *self, void *in, void *out) {
    (void)self; (void)in;
    fs_event_t *o = (fs_event_t *)out;
    fe_memset(o, 0, sizeof(*o));
    if (!s_pending_valid) { o->valid = 0; return; }
    o->kind = s_pending_req.kind;
    if (s_pending_req.path)     fe_strncpy(o->path,     s_pending_req.path,     FS_EVENT_PATH_MAX);
    if (s_pending_req.new_path) fe_strncpy(o->new_path, s_pending_req.new_path, FS_EVENT_PATH_MAX);
    o->size  = s_pending_req.size;
    o->tsc   = timer_read_tsc();
    o->timestamp = tod_ready() ? tod_now_unix() : 0;
    o->seq   = ++s_seq;
    o->valid = 1;
}

static void node_validate(chain_node_t *self, void *in, void *out) {
    (void)self;
    fs_event_t *i = (fs_event_t *)in;
    fs_event_t *o = (fs_event_t *)out;
    *o = *i;
    if (!i->valid) return;
    if (i->kind <= FS_EV_NONE || i->kind > FS_EV_RENAME) { o->valid = 0; return; }
    if (i->path[0] == 0) { o->valid = 0; return; }
}

static void node_execute(chain_node_t *self, void *in, void *out) {
    (void)self;
    fs_event_t *i = (fs_event_t *)in;
    fs_event_t *o = (fs_event_t *)out;
    *o = *i;
    /* No-op: the underlying fat32_* mutation already happened on the
     * caller's side. The emit here records the event into the chain
     * for subscribers. Future shape moves the mutation here. */
}

static void node_emit_event(chain_node_t *self, void *in, void *out) {
    (void)self;
    fs_event_t *i = (fs_event_t *)in;
    fs_event_t *o = (fs_event_t *)out;
    *o = *i;
    if (!i->valid) return;

    s_last_event = *i;
    g_total_emits++;
    s_pending_valid = 0;

    /* Bump emitter chain's vault_version per CHAIN_CONTRACT. */
    if (CHAIN_FS_EVENT >= 0) {
        chain_t *c = chain_get(CHAIN_FS_EVENT);
        if (c) c->vault_version++;
    }

    /* UI bridge listeners (non-chain consumers). */
    fire_listeners(i);
}

/* ─────────────────────────────────────────────────────────────────
 * Subscriber: TRASH_REACT  (fs_event -> match_delete -> log)
 * Behavior: when an UNLINK on a >=100MB file is observed, log a
 * record to a small in-RAM ring + bump counter. The brief allows
 * "best-effort" retro-trash; we honestly log rather than mutate
 * post-hoc (the file is already gone, retro-trash would require
 * journal-driven recovery which lives in masq_journal, not here).
 * ───────────────────────────────────────────────────────────────── */

#define TRASH_REACT_LOG_CAP 16
typedef struct {
    char     path[FS_EVENT_PATH_MAX];
    uint32_t size;
    uint64_t timestamp;
} trash_react_rec_t;
static trash_react_rec_t s_tr_log[TRASH_REACT_LOG_CAP];
static int               s_tr_head;

static void node_tr_input(chain_node_t *self, void *in, void *out) {
    (void)self; (void)in;
    fs_event_t *o = (fs_event_t *)out;
    *o = s_last_event;
}

static void node_tr_match_delete(chain_node_t *self, void *in, void *out) {
    (void)self;
    fs_event_t *i = (fs_event_t *)in;
    fs_event_t *o = (fs_event_t *)out;
    *o = *i;
    if (!i->valid) { o->valid = 0; return; }
    if (i->kind != FS_EV_UNLINK) { o->valid = 0; return; }
    /* >=100MB threshold per brief. */
    if (i->size < (100u * 1024u * 1024u)) { o->valid = 0; return; }
}

static void node_tr_log(chain_node_t *self, void *in, void *out) {
    (void)self;
    fs_event_t *i = (fs_event_t *)in;
    fs_event_t *o = (fs_event_t *)out;
    *o = *i;
    if (!i->valid) return;
    fe_strncpy(s_tr_log[s_tr_head].path, i->path, FS_EVENT_PATH_MAX);
    s_tr_log[s_tr_head].size      = i->size;
    s_tr_log[s_tr_head].timestamp = i->timestamp;
    s_tr_head = (s_tr_head + 1) % TRASH_REACT_LOG_CAP;
    g_total_trash_reacts++;
    if (CHAIN_FS_TRASH_REACT >= 0) {
        chain_t *c = chain_get(CHAIN_FS_TRASH_REACT);
        if (c) c->vault_version++;
    }
}

/* ─────────────────────────────────────────────────────────────────
 * Subscriber: INDEX  (fs_event -> bucket_path -> update_count)
 * Stub behavior: count events per parent directory. Exposed via
 * a small bounded table. Future: full-text inverted index.
 * ───────────────────────────────────────────────────────────────── */

#define INDEX_DIRS_MAX 32
typedef struct {
    char     dir[FS_EVENT_PATH_MAX];
    uint32_t event_count;
    int      used;
} index_bucket_t;
static index_bucket_t s_index_buckets[INDEX_DIRS_MAX];

/* Carry the bucket index between nodes via the unused 'prior_kind' on
 * the in-flight signal copy. Crude but matches the pass-through type
 * contract. -1 means "no slot found / overflow, count globally only". */
static void node_idx_input(chain_node_t *self, void *in, void *out) {
    (void)self; (void)in;
    fs_event_t *o = (fs_event_t *)out;
    *o = s_last_event;
}

static void node_idx_bucket_path(chain_node_t *self, void *in, void *out) {
    (void)self;
    fs_event_t *i = (fs_event_t *)in;
    fs_event_t *o = (fs_event_t *)out;
    *o = *i;
    if (!i->valid) return;

    char dir[FS_EVENT_PATH_MAX];
    fe_parent_dir(i->path, dir, FS_EVENT_PATH_MAX);

    /* Find or allocate bucket. */
    int slot = -1;
    for (int k = 0; k < INDEX_DIRS_MAX; k++) {
        if (s_index_buckets[k].used && fe_streq(s_index_buckets[k].dir, dir)) {
            slot = k; break;
        }
    }
    if (slot < 0) {
        for (int k = 0; k < INDEX_DIRS_MAX; k++) {
            if (!s_index_buckets[k].used) {
                fe_strncpy(s_index_buckets[k].dir, dir, FS_EVENT_PATH_MAX);
                s_index_buckets[k].event_count = 0;
                s_index_buckets[k].used = 1;
                slot = k; break;
            }
        }
    }
    /* Overflow: drop into bucket 0 as best-effort (honest stub). */
    if (slot < 0) slot = 0;
    o->prior_kind = slot;  /* repurposed as bucket id for the next node */
}

static void node_idx_update_count(chain_node_t *self, void *in, void *out) {
    (void)self;
    fs_event_t *i = (fs_event_t *)in;
    fs_event_t *o = (fs_event_t *)out;
    *o = *i;
    if (!i->valid) return;
    int slot = i->prior_kind;
    if (slot >= 0 && slot < INDEX_DIRS_MAX && s_index_buckets[slot].used) {
        s_index_buckets[slot].event_count++;
    }
    g_total_index_updates++;
    if (CHAIN_FS_INDEX >= 0) {
        chain_t *c = chain_get(CHAIN_FS_INDEX);
        if (c) c->vault_version++;
    }
}

/* ─────────────────────────────────────────────────────────────────
 * Subscriber: UNDO  (fs_event -> derive_reverse -> push_ring)
 *
 * 32-entry ring of reverse ops. fs_event_undo_pop() applies the
 * most recent reverse op. Reversible kinds:
 *   CREATE  -> unlink
 *   MKDIR   -> unlink (rmdir)
 *   TRASH   -> restore by id
 *   UNLINK  -> not reversible (data already gone); record but pop returns -1
 *   WRITE/TRUNCATE -> not reversible at this layer (would need journal)
 * ───────────────────────────────────────────────────────────────── */

#define UNDO_RING_CAP 32
typedef struct {
    int      original_kind;
    char     path[FS_EVENT_PATH_MAX];
    char     new_path[FS_EVENT_PATH_MAX];
    uint32_t size;
    uint64_t timestamp;
    int      reversible;
} undo_rec_t;
static undo_rec_t s_undo_ring[UNDO_RING_CAP];
static int        s_undo_head;
static int        s_undo_count;

static int kind_is_reversible(int k) {
    return k == FS_EV_CREATE || k == FS_EV_MKDIR || k == FS_EV_TRASH ||
           k == FS_EV_RESTORE;
}

static void node_undo_input(chain_node_t *self, void *in, void *out) {
    (void)self; (void)in;
    fs_event_t *o = (fs_event_t *)out;
    *o = s_last_event;
}

static void node_undo_derive_reverse(chain_node_t *self, void *in, void *out) {
    (void)self;
    fs_event_t *i = (fs_event_t *)in;
    fs_event_t *o = (fs_event_t *)out;
    *o = *i;
    /* No mutation; classification happens in the next node via flag in
     * prior_kind == 1 means reversible. */
    if (!i->valid) return;
    o->prior_kind = kind_is_reversible(i->kind) ? 1 : 0;
}

static void node_undo_push_ring(chain_node_t *self, void *in, void *out) {
    (void)self;
    fs_event_t *i = (fs_event_t *)in;
    fs_event_t *o = (fs_event_t *)out;
    *o = *i;
    if (!i->valid) return;
    undo_rec_t *r = &s_undo_ring[s_undo_head];
    r->original_kind = i->kind;
    fe_strncpy(r->path,     i->path,     FS_EVENT_PATH_MAX);
    fe_strncpy(r->new_path, i->new_path, FS_EVENT_PATH_MAX);
    r->size        = i->size;
    r->timestamp   = i->timestamp;
    r->reversible  = i->prior_kind == 1;
    s_undo_head = (s_undo_head + 1) % UNDO_RING_CAP;
    if (s_undo_count < UNDO_RING_CAP) s_undo_count++;
    g_total_undo_records++;
    if (CHAIN_FS_UNDO >= 0) {
        chain_t *c = chain_get(CHAIN_FS_UNDO);
        if (c) c->vault_version++;
    }
}

int fs_event_undo_count(void) { return s_undo_count; }

int fs_event_undo_pop(void) {
    if (s_undo_count == 0) return -1;
    /* newest record sits at (head - 1 + CAP) % CAP */
    int idx = (s_undo_head - 1 + UNDO_RING_CAP) % UNDO_RING_CAP;
    undo_rec_t *r = &s_undo_ring[idx];
    int rc = -1;
    if (r->reversible) {
        switch (r->original_kind) {
            case FS_EV_CREATE:
            case FS_EV_MKDIR:
                rc = fat32_unlink(r->path);
                break;
            case FS_EV_TRASH:
                /* new_path holds the trash id */
                rc = fat32_trash_restore(r->new_path);
                break;
            case FS_EV_RESTORE:
                /* reverse a restore = trash again */
                rc = fat32_trash(r->path, "undo", 0);
                break;
            default:
                rc = -1;
        }
    }
    /* Always consume the ring slot to advance, regardless of whether
     * the underlying op succeeded (avoids retry-spam on the same
     * unreversible op). */
    s_undo_head = idx;
    s_undo_count--;
    return rc;
}

/* ─────────────────────────────────────────────────────────────────
 * Subscriber: NOTIFY  (fs_event -> classify -> notify_send)
 * Fires notifications for:
 *   - large delete (>=500MB) ............ NOTIFY_WARNING
 *   - large trash empty (TRASH_EMPTY) ... NOTIFY_INFO (placeholder)
 * Future hook: low-disk-space detection once df is plumbed in.
 * ───────────────────────────────────────────────────────────────── */

static void node_notif_input(chain_node_t *self, void *in, void *out) {
    (void)self; (void)in;
    fs_event_t *o = (fs_event_t *)out;
    *o = s_last_event;
}

static void node_notif_classify(chain_node_t *self, void *in, void *out) {
    (void)self;
    fs_event_t *i = (fs_event_t *)in;
    fs_event_t *o = (fs_event_t *)out;
    *o = *i;
    if (!i->valid) { o->valid = 0; return; }
    int interesting = 0;
    if (i->kind == FS_EV_UNLINK && i->size >= (500u * 1024u * 1024u)) interesting = 1;
    if (!interesting) o->valid = 0;
}

static void node_notif_send(chain_node_t *self, void *in, void *out) {
    (void)self;
    fs_event_t *i = (fs_event_t *)in;
    fs_event_t *o = (fs_event_t *)out;
    *o = *i;
    if (!i->valid) return;

    char msg[NOTIFY_MAX_TEXT];
    int o2 = 0;
    const char *p = "deleted: ";
    while (*p && o2 < (int)sizeof(msg) - 1) msg[o2++] = *p++;
    for (int k = 0; i->path[k] && o2 < (int)sizeof(msg) - 1; k++) msg[o2++] = i->path[k];
    msg[o2] = 0;
    notify_send(msg, "fs", NOTIFY_WARNING);
    g_total_notify_fires++;
    if (CHAIN_FS_NOTIFY >= 0) {
        chain_t *c = chain_get(CHAIN_FS_NOTIFY);
        if (c) c->vault_version++;
    }
}

/* ─────────────────────────────────────────────────────────────────
 * Public emit entrypoint
 * ───────────────────────────────────────────────────────────────── */

void fs_event_emit(int kind, const char *path, uint32_t size,
                   int prior_kind)
{
    if (CHAIN_FS_EVENT < 0) return;       /* registry not yet up */
    if (!path || !path[0]) return;
    s_pending_req.kind     = kind;
    s_pending_req.path     = path;
    s_pending_req.new_path = 0;
    s_pending_req.buf      = 0;
    s_pending_req.size     = size;
    s_pending_req.offset   = 0;
    s_pending_valid        = 1;

    /* Resolve emitter so the validate -> execute -> emit_event chain
     * runs and s_last_event becomes the just-emitted record. */
    chain_resolve(CHAIN_FS_EVENT);
    /* prior_kind overrides whatever validate stamped (used by undo). */
    s_last_event.prior_kind = prior_kind;

    /* Pump subscribers. MDE auto_route also wires these because they
     * declare input_type=fs_event matching the emitter's output_type;
     * we explicit-pump too so events fire synchronously regardless of
     * scheduler tick timing. */
    if (CHAIN_FS_TRASH_REACT >= 0) chain_resolve(CHAIN_FS_TRASH_REACT);
    if (CHAIN_FS_INDEX       >= 0) chain_resolve(CHAIN_FS_INDEX);
    if (CHAIN_FS_UNDO        >= 0) chain_resolve(CHAIN_FS_UNDO);
    if (CHAIN_FS_NOTIFY      >= 0) chain_resolve(CHAIN_FS_NOTIFY);
}

static void emit_with_kind(int kind, const char *path,
                           const char *new_path, uint32_t size)
{
    if (CHAIN_FS_EVENT < 0) return;
    if (!path) return;
    s_pending_req.kind     = kind;
    s_pending_req.path     = path;
    s_pending_req.new_path = new_path;
    s_pending_req.size     = size;
    s_pending_valid        = 1;
    chain_resolve(CHAIN_FS_EVENT);
    if (CHAIN_FS_TRASH_REACT >= 0) chain_resolve(CHAIN_FS_TRASH_REACT);
    if (CHAIN_FS_INDEX       >= 0) chain_resolve(CHAIN_FS_INDEX);
    if (CHAIN_FS_UNDO        >= 0) chain_resolve(CHAIN_FS_UNDO);
    if (CHAIN_FS_NOTIFY      >= 0) chain_resolve(CHAIN_FS_NOTIFY);
}

void fs_event_emit_rename(const char *old_path, const char *new_path,
                          uint32_t size)
{
    emit_with_kind(FS_EV_RENAME, old_path, new_path, size);
}

void fs_event_emit_trash(const char *path, const char *id, uint32_t size)
{
    emit_with_kind(FS_EV_TRASH, path, id, size);
}

void fs_event_emit_restore(const char *path, const char *id, uint32_t size)
{
    emit_with_kind(FS_EV_RESTORE, path, id, size);
}

/* ─────────────────────────────────────────────────────────────────
 * Chain registration
 * ───────────────────────────────────────────────────────────────── */

int fs_event_chain_register(int parent_chain_id) {
    if (CHAIN_FS_EVENT >= 0) return 5;

    int e = chain_create("fs_event", parent_chain_id, MASQ_INTERNAL);
    if (e < 0) return -1;
    chain_add_node(e, "fs_request", "void",      "fs_event", node_fs_request);
    chain_add_node(e, "validate",   "fs_event",  "fs_event", node_validate);
    chain_add_node(e, "execute",    "fs_event",  "fs_event", node_execute);
    chain_add_node(e, "emit_event", "fs_event",  "fs_event", node_emit_event);
    CHAIN_FS_EVENT = e;

    int tr = chain_create("fs_trash_react", parent_chain_id, MASQ_INTERNAL);
    if (tr >= 0) {
        chain_add_node(tr, "input",        "fs_event", "fs_event", node_tr_input);
        chain_add_node(tr, "match_delete", "fs_event", "fs_event", node_tr_match_delete);
        chain_add_node(tr, "log",          "fs_event", "fs_event", node_tr_log);
        CHAIN_FS_TRASH_REACT = tr;
    }

    int idx = chain_create("fs_index", parent_chain_id, MASQ_INTERNAL);
    if (idx >= 0) {
        chain_add_node(idx, "input",        "fs_event", "fs_event", node_idx_input);
        chain_add_node(idx, "bucket_path",  "fs_event", "fs_event", node_idx_bucket_path);
        chain_add_node(idx, "update_count", "fs_event", "fs_event", node_idx_update_count);
        CHAIN_FS_INDEX = idx;
    }

    int u = chain_create("fs_undo", parent_chain_id, MASQ_INTERNAL);
    if (u >= 0) {
        chain_add_node(u, "input",          "fs_event", "fs_event", node_undo_input);
        chain_add_node(u, "derive_reverse", "fs_event", "fs_event", node_undo_derive_reverse);
        chain_add_node(u, "push_ring",      "fs_event", "fs_event", node_undo_push_ring);
        CHAIN_FS_UNDO = u;
    }

    int n = chain_create("fs_notify", parent_chain_id, MASQ_INTERNAL);
    if (n >= 0) {
        chain_add_node(n, "input",      "fs_event", "fs_event", node_notif_input);
        chain_add_node(n, "classify",   "fs_event", "fs_event", node_notif_classify);
        chain_add_node(n, "notify_send","fs_event", "fs_event", node_notif_send);
        CHAIN_FS_NOTIFY = n;
    }

    int total = 0;
    if (CHAIN_FS_EVENT       >= 0) total++;
    if (CHAIN_FS_TRASH_REACT >= 0) total++;
    if (CHAIN_FS_INDEX       >= 0) total++;
    if (CHAIN_FS_UNDO        >= 0) total++;
    if (CHAIN_FS_NOTIFY      >= 0) total++;
    return total;
}
