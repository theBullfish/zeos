/*
 * CFA identity contexts. Each context is a CFA root with its own
 * MasQ tier policy, chain visibility set, disk encryption key,
 * and namespace. Login = switching the active root. The Zeos
 * primitive replaces UNIX-style users / UIDs / sudo entirely.
 *
 * Cross-context perception is limited to REFERENCE-tier chains;
 * INTERNAL and SOVEREIGN are scoped to their own context.
 *
 * Storage layout in VAULT:
 *   /identity/index            uint32 next_id, uint32 active_id, ...
 *   /identity/<id>/meta        identity_record_t
 *   /identity/<id>/pin         ASCII digits, NUL-padded (SOVEREIGN tier)
 *   /identity/<id>/salt        32B PBKDF2 salt for the per-context key
 *
 * The active context's CFA root is exposed via identity_active_cfa_root.
 * chain.c:chain_create derives every new chain's addr from this root,
 * and chain.c:chain_can_perceive consults identity_roots_same_context
 * before allowing INTERNAL or SOVEREIGN reads across contexts.
 */

#include "identity.h"
#include "vault.h"
#include "chain.h"
#include "cfa_handle.h"
#include "crypto_disk.h"
#include "kprint.h"
#include "timer.h"
#include "spinlock.h"

#include <stdint.h>
#include <stddef.h>

/* ── Local string helpers (freestanding) ─────────────────────────── */

static int id_strlen(const char *s) { int n=0; while (s[n]) n++; return n; }
static int id_streq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == '\0' && *b == '\0';
}
static void id_strncpy(char *dst, const char *src, int cap) {
    int i = 0; if (cap <= 0) return;
    while (i < cap - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}
static void id_memzero(void *p, int n) {
    volatile uint8_t *v = (volatile uint8_t *)p;
    for (int i = 0; i < n; i++) v[i] = 0;
}
static int id_is_digit(char c) { return c >= '0' && c <= '9'; }

/* Constant-time PIN compare across the full PIN buffer. Both buffers
 * are NUL-padded to the same fixed length so leaking the prefix-match
 * length via short-circuit isn't possible. */
static int id_pin_compare_ct(const char *a, const char *b, int len)
{
    unsigned diff = 0;
    for (int i = 0; i < len; i++) {
        diff |= (unsigned)((unsigned char)a[i] ^ (unsigned char)b[i]);
    }
    return diff == 0;
}

/* ── State ───────────────────────────────────────────────────────── */

#define ID_PIN_BUF (IDENTITY_PIN_MAX_LEN + 1)

/* Per-slot PIN backing buffer (CFA-wrapped at SOVEREIGN). The buffer
 * lives here, never escapes via cfa_resolve except during a context
 * switch verify. */
typedef struct {
    int          used;
    int          id;
    char         name[IDENTITY_NAME_MAX];
    cfa_addr_t   cfa_root_addr;
    masq_tier_t  masq_tier_default;
    int          owner_chains[IDENTITY_OWNER_CHAINS_MAX];
    int          owner_chains_count;
    uint64_t     created_tsc;
    uint64_t     last_active_tsc;

    /* PIN. The handle is set up lazily on first use to avoid a wrap
     * before the chain registry is alive. */
    char         pin_buf[ID_PIN_BUF];
    cfa_handle_t pin_h;
} id_slot_t;

static id_slot_t       g_slots[IDENTITY_MAX_CONTEXTS];
static int             g_active_id;       /* 0 = none */
static int             g_initialized;
static zeos_spinlock_t g_id_lock = ZEOS_SPINLOCK_INIT;

/* ── VAULT path builders ─────────────────────────────────────────── */

static void path_meta(int id, char *out, int cap) {
    /* /identity/<id>/meta */
    int i = 0;
    const char *p = "/identity/";
    while (*p && i < cap - 1) out[i++] = *p++;
    /* id */
    char num[8]; int n = 0;
    int v = id; if (v == 0) { num[n++] = '0'; }
    while (v > 0 && n < 7) { num[n++] = '0' + (v % 10); v /= 10; }
    while (n > 0 && i < cap - 1) out[i++] = num[--n];
    p = "/meta";
    while (*p && i < cap - 1) out[i++] = *p++;
    out[i] = '\0';
}
static void path_pin(int id, char *out, int cap) {
    int i = 0;
    const char *p = "/identity/";
    while (*p && i < cap - 1) out[i++] = *p++;
    char num[8]; int n = 0;
    int v = id; if (v == 0) { num[n++] = '0'; }
    while (v > 0 && n < 7) { num[n++] = '0' + (v % 10); v /= 10; }
    while (n > 0 && i < cap - 1) out[i++] = num[--n];
    p = "/pin";
    while (*p && i < cap - 1) out[i++] = *p++;
    out[i] = '\0';
}
static void path_salt(int id, char *out, int cap) {
    int i = 0;
    const char *p = "/identity/";
    while (*p && i < cap - 1) out[i++] = *p++;
    char num[8]; int n = 0;
    int v = id; if (v == 0) { num[n++] = '0'; }
    while (v > 0 && n < 7) { num[n++] = '0' + (v % 10); v /= 10; }
    while (n > 0 && i < cap - 1) out[i++] = num[--n];
    p = "/salt";
    while (*p && i < cap - 1) out[i++] = *p++;
    out[i] = '\0';
}

/* On-disk meta record. */
typedef struct {
    uint32_t magic;            /* 'IDC1' */
    uint32_t id;
    char     name[IDENTITY_NAME_MAX];
    uint32_t cfa_root_seg0;    /* unique per id */
    uint32_t masq_tier_default;
    uint64_t created_tsc;
    uint64_t last_active_tsc;
    uint8_t  reserved[24];
} __attribute__((packed)) id_meta_record_t;

#define IDENTITY_META_MAGIC 0x31434449u  /* 'IDC1' little-endian */

/* Index record: tracks which slots are live and the most-recently-active
 * context id (so a cold boot can pre-select the right context). */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t live_mask;        /* bit i set => slot id (i+1) is live */
    uint32_t last_active_id;
    uint8_t  reserved[16];
} __attribute__((packed)) id_index_record_t;

#define IDENTITY_INDEX_MAGIC 0x49445831u  /* 'IDX1' */
#define IDENTITY_INDEX_PATH  "/identity/index"

/* ── Slot helpers ────────────────────────────────────────────────── */

static id_slot_t *slot_by_id(int id) {
    for (int i = 0; i < IDENTITY_MAX_CONTEXTS; i++)
        if (g_slots[i].used && g_slots[i].id == id) return &g_slots[i];
    return 0;
}

static int slot_alloc(void) {
    /* Smallest unused id starting at 1. */
    for (int candidate = 1; candidate <= IDENTITY_MAX_CONTEXTS; candidate++) {
        if (slot_by_id(candidate)) continue;
        for (int i = 0; i < IDENTITY_MAX_CONTEXTS; i++) {
            if (!g_slots[i].used) {
                id_memzero(&g_slots[i], (int)sizeof(id_slot_t));
                g_slots[i].used = 1;
                g_slots[i].id = candidate;
                return i;
            }
        }
    }
    return -1;
}

static void slot_wrap_pin(id_slot_t *s) {
    if (!s || s->pin_h) return;
    cfa_addr_t base = s->cfa_root_addr;
    cfa_addr_t a = cfa_derive(&base, 0x91D);  /* 'PID' */
    s->pin_h = cfa_wrap(s->pin_buf, sizeof(s->pin_buf), a, MASQ_SOVEREIGN);
    /* On wrap failure we fall back to direct buffer access; the wrap
     * is a defence-in-depth layer, not a hard invariant. */
}

static char *pin_resolve_or_fallback(id_slot_t *s) {
    if (!s) return 0;
    if (s->pin_h == 0) return s->pin_buf;
    int prev_obs = cfa_get_observer();
    cfa_set_observer(-1);
    void *p = cfa_resolve(s->pin_h);
    cfa_set_observer(prev_obs);
    return p ? (char *)p : s->pin_buf;
}

/* ── CFA root for a context ─────────────────────────────────────── */

static cfa_addr_t make_root_for_id(int id) {
    cfa_addr_t a;
    for (int i = 0; i < 8; i++) a.segments[i] = 0;
    a.segments[0] = IDENTITY_ROOT_BASE + (uint32_t)id;
    a.depth = 1;
    a.birth_tsc = timer_read_tsc();
    return a;
}

/* ── Persistence ─────────────────────────────────────────────────── */

static int meta_save(id_slot_t *s) {
    char p[64];
    path_meta(s->id, p, sizeof(p));
    id_meta_record_t rec;
    id_memzero(&rec, (int)sizeof(rec));
    rec.magic = IDENTITY_META_MAGIC;
    rec.id = (uint32_t)s->id;
    id_strncpy(rec.name, s->name, IDENTITY_NAME_MAX);
    rec.cfa_root_seg0 = s->cfa_root_addr.segments[0];
    rec.masq_tier_default = (uint32_t)s->masq_tier_default;
    rec.created_tsc = s->created_tsc;
    rec.last_active_tsc = s->last_active_tsc;
    int wrote = vault_save_config(p, &rec, sizeof(rec));
    return (wrote == (int)sizeof(rec)) ? 0 : -1;
}

static int meta_load(int id, id_slot_t *out) {
    char p[64];
    path_meta(id, p, sizeof(p));
    id_meta_record_t rec;
    int got = vault_load_config(p, &rec, sizeof(rec));
    if (got != (int)sizeof(rec)) return -1;
    if (rec.magic != IDENTITY_META_MAGIC) return -1;
    if (rec.id != (uint32_t)id) return -1;

    id_strncpy(out->name, rec.name, IDENTITY_NAME_MAX);
    /* Reconstruct CFA root deterministically from the id (so segment[0]
     * always matches across reboots regardless of stored seg0). */
    out->cfa_root_addr = make_root_for_id(id);
    /* Preserve the persisted seg0 if it differs (older record). */
    if (rec.cfa_root_seg0)
        out->cfa_root_addr.segments[0] = rec.cfa_root_seg0;
    out->masq_tier_default = (masq_tier_t)rec.masq_tier_default;
    out->created_tsc = rec.created_tsc;
    out->last_active_tsc = rec.last_active_tsc;
    return 0;
}

static int index_save(void) {
    id_index_record_t idx;
    id_memzero(&idx, (int)sizeof(idx));
    idx.magic = IDENTITY_INDEX_MAGIC;
    idx.version = 1;
    idx.last_active_id = (uint32_t)g_active_id;
    uint32_t mask = 0;
    for (int i = 0; i < IDENTITY_MAX_CONTEXTS; i++) {
        if (g_slots[i].used) {
            int bit = g_slots[i].id - 1;
            if (bit >= 0 && bit < 32) mask |= (1u << bit);
        }
    }
    idx.live_mask = mask;
    int wrote = vault_save_config(IDENTITY_INDEX_PATH, &idx, sizeof(idx));
    return (wrote == (int)sizeof(idx)) ? 0 : -1;
}

static int index_load(uint32_t *live_mask, uint32_t *last_active_id) {
    id_index_record_t idx;
    int got = vault_load_config(IDENTITY_INDEX_PATH, &idx, sizeof(idx));
    if (got != (int)sizeof(idx)) return -1;
    if (idx.magic != IDENTITY_INDEX_MAGIC) return -1;
    *live_mask = idx.live_mask;
    *last_active_id = idx.last_active_id;
    return 0;
}

/* PIN persistence: store as raw ASCII digits in a vault key. The
 * vault tier (SOVEREIGN) signals the data is sensitive; once
 * crypto_disk is armed the vault layer routes /identity/...  through
 * encryption transparently for callers using vault_put_encrypted. */
static int pin_save(int id, const char *pin, int pin_len) {
    char p[64];
    path_pin(id, p, sizeof(p));
    char buf[ID_PIN_BUF];
    id_memzero(buf, sizeof(buf));
    int n = pin_len;
    if (n > IDENTITY_PIN_MAX_LEN) n = IDENTITY_PIN_MAX_LEN;
    for (int i = 0; i < n; i++) buf[i] = pin[i];
    int wrote = vault_write(p, buf, (uint32_t)n);
    id_memzero(buf, sizeof(buf));
    return (wrote >= 0) ? 0 : -1;
}

static int pin_load_into(int id, char *out, int out_cap) {
    char p[64];
    path_pin(id, p, sizeof(p));
    if (out_cap < ID_PIN_BUF) return -1;
    id_memzero(out, ID_PIN_BUF);
    int got = vault_read(p, out, IDENTITY_PIN_MAX_LEN);
    if (got <= 0) return -1;
    return got;
}

/* ── chain.c bridge ───────────────────────────────────────────────
 *
 * chain.c calls identity_active_cfa_root() at chain_create time and
 * uses it as the parent for any new chain whose caller passes
 * parent_id == -1. The mechanism is opt-in -- chain_create's existing
 * "root chain" path stays so boot-time hardware chains created before
 * any context exists still register cleanly.
 */

cfa_addr_t identity_active_cfa_root(void) {
    cfa_addr_t z;
    for (int i = 0; i < 8; i++) z.segments[i] = 0;
    z.depth = 0; z.birth_tsc = 0;
    if (g_active_id == 0) return z;
    id_slot_t *s = slot_by_id(g_active_id);
    if (!s) return z;
    return s->cfa_root_addr;
}

int identity_context_active(void) { return g_active_id; }

int identity_roots_same_context(const cfa_addr_t *a, const cfa_addr_t *b) {
    if (!a || !b) return 1;  /* permissive when uninitialised */
    /* Identity context root segment[0] == IDENTITY_ROOT_BASE + id. If
     * neither address has a segment[0] in that range, they are non-
     * context-rooted (boot-time hardware chains, root system chains)
     * and we don't apply context scoping. */
    uint32_t a0 = a->segments[0];
    uint32_t b0 = b->segments[0];
    int a_id = (a0 >= IDENTITY_ROOT_BASE
                && a0 < IDENTITY_ROOT_BASE + IDENTITY_MAX_CONTEXTS + 1u)
               ? (int)(a0 - IDENTITY_ROOT_BASE) : 0;
    int b_id = (b0 >= IDENTITY_ROOT_BASE
                && b0 < IDENTITY_ROOT_BASE + IDENTITY_MAX_CONTEXTS + 1u)
               ? (int)(b0 - IDENTITY_ROOT_BASE) : 0;
    if (a_id == 0 && b_id == 0) return 1;  /* neither is context-rooted */
    if (a_id == 0 || b_id == 0) return 1;  /* one side is system / pre-id */
    return a_id == b_id;
}

void identity_register_chain(int chain_id) {
    if (g_active_id == 0) return;
    spin_lock(&g_id_lock);
    id_slot_t *s = slot_by_id(g_active_id);
    if (s && s->owner_chains_count < IDENTITY_OWNER_CHAINS_MAX) {
        s->owner_chains[s->owner_chains_count++] = chain_id;
    }
    spin_unlock(&g_id_lock);
}

/* ── Listing ─────────────────────────────────────────────────────── */

int identity_context_list(void (*cb)(const identity_context_t *, void *), void *user)
{
    int n = 0;
    for (int i = 0; i < IDENTITY_MAX_CONTEXTS; i++) {
        id_slot_t *s = &g_slots[i];
        if (!s->used) continue;
        if (cb) {
            identity_context_t snap;
            id_memzero(&snap, (int)sizeof(snap));
            snap.id = s->id;
            id_strncpy(snap.name, s->name, IDENTITY_NAME_MAX);
            snap.cfa_root_addr = s->cfa_root_addr;
            snap.masq_tier_default = s->masq_tier_default;
            snap.pin_handle = s->pin_h;
            for (int j = 0; j < s->owner_chains_count
                            && j < IDENTITY_OWNER_CHAINS_MAX; j++)
                snap.owner_chains[j] = s->owner_chains[j];
            snap.owner_chains_count = s->owner_chains_count;
            snap.created_tsc = s->created_tsc;
            snap.last_active_tsc = s->last_active_tsc;
            cb(&snap, user);
        }
        n++;
    }
    return n;
}

int identity_context_by_name(const char *name) {
    if (!name) return -1;
    for (int i = 0; i < IDENTITY_MAX_CONTEXTS; i++) {
        if (!g_slots[i].used) continue;
        if (id_streq(g_slots[i].name, name)) return g_slots[i].id;
    }
    return -1;
}

/* ── Create ──────────────────────────────────────────────────────── */

static int validate_pin(const char *pin) {
    if (!pin) return 0;
    int n = id_strlen(pin);
    if (n < IDENTITY_PIN_MIN_LEN || n > IDENTITY_PIN_MAX_LEN) return 0;
    for (int i = 0; i < n; i++)
        if (!id_is_digit(pin[i])) return 0;
    return n;
}

int identity_context_create(const char *name, const char *pin)
{
    if (!name || !*name) return -1;
    if (id_strlen(name) >= IDENTITY_NAME_MAX) return -1;
    int pin_len = validate_pin(pin);
    if (pin_len == 0) return -1;
    if (identity_context_by_name(name) >= 0) return -1;

    spin_lock(&g_id_lock);
    int idx = slot_alloc();
    if (idx < 0) { spin_unlock(&g_id_lock); return -1; }
    id_slot_t *s = &g_slots[idx];
    id_strncpy(s->name, name, IDENTITY_NAME_MAX);
    s->cfa_root_addr = make_root_for_id(s->id);
    s->masq_tier_default = MASQ_INTERNAL;
    s->owner_chains_count = 0;
    s->created_tsc = timer_read_tsc();
    s->last_active_tsc = 0;

    /* Stash the PIN bytes in the slot (NUL-padded), wrap with CFA. */
    id_memzero(s->pin_buf, sizeof(s->pin_buf));
    for (int i = 0; i < pin_len; i++) s->pin_buf[i] = pin[i];
    slot_wrap_pin(s);

    int new_id = s->id;
    spin_unlock(&g_id_lock);

    /* Persist outside the lock (vault calls may take their own lock). */
    if (meta_save(s) < 0)         { goto fail; }
    if (pin_save(new_id, pin, pin_len) < 0) { goto fail; }
    if (index_save() < 0)         { goto fail; }

    kputs("[identity] created context \"");
    kputs(s->name);
    kputs("\" id=");
    kput_dec((uint64_t)new_id);
    kputc('\n');
    return new_id;

fail:
    spin_lock(&g_id_lock);
    id_memzero(s->pin_buf, sizeof(s->pin_buf));
    if (s->pin_h) { cfa_release(s->pin_h); s->pin_h = 0; }
    s->used = 0;
    spin_unlock(&g_id_lock);
    return -1;
}

/* ── Switch ──────────────────────────────────────────────────────── */

int identity_context_switch(int id, const char *pin)
{
    if (id <= 0) return -1;
    int pin_len = validate_pin(pin);
    if (pin_len == 0) return -1;

    id_slot_t *s = slot_by_id(id);
    if (!s) return -1;

    /* Constant-time compare across the full PIN buffer. */
    char stored[ID_PIN_BUF];
    id_memzero(stored, sizeof(stored));
    int got = pin_load_into(id, stored, sizeof(stored));
    if (got <= 0) {
        /* Fall back to in-RAM buffer (just-created context whose vault
         * write already happened in pin_save). */
        char *buf = pin_resolve_or_fallback(s);
        for (int i = 0; i < ID_PIN_BUF; i++) stored[i] = buf[i];
    }

    char attempt[ID_PIN_BUF];
    id_memzero(attempt, sizeof(attempt));
    for (int i = 0; i < pin_len && i < IDENTITY_PIN_MAX_LEN; i++)
        attempt[i] = pin[i];

    int ok = id_pin_compare_ct(stored, attempt, ID_PIN_BUF);
    id_memzero(attempt, sizeof(attempt));
    if (!ok) {
        id_memzero(stored, sizeof(stored));
        kputs("[identity] switch DENIED (bad PIN, id=");
        kput_dec((uint64_t)id);
        kputs(")\n");
        return -1;
    }

    int prev = g_active_id;
    spin_lock(&g_id_lock);
    g_active_id = id;
    s->last_active_tsc = timer_read_tsc();
    spin_unlock(&g_id_lock);

    /* Re-derive disk encryption key from this context's PIN + salt.
     * The salt path is per-context; crypto_disk creates the salt on
     * first switch into a context that doesn't have one yet. */
    char salt_path[64];
    path_salt(id, salt_path, sizeof(salt_path));
    int rc = crypto_disk_reinit_with_salt(stored, pin_len, salt_path);
    id_memzero(stored, sizeof(stored));
    if (rc != 0) {
        /* Auth succeeded but key derivation failed -- restore and bail. */
        spin_lock(&g_id_lock);
        g_active_id = prev;
        spin_unlock(&g_id_lock);
        kputs("[identity] WARN: crypto_disk reinit failed; rollback\n");
        return -1;
    }

    (void)meta_save(s);
    (void)index_save();

    /* MasQ journal entry. We use kputs + bumping CHAIN_BLOCK
     * vault_version so the change is visible to anyone tailing the
     * masq journal feed. */
    kputs("[identity] switched ");
    kput_dec((uint64_t)prev);
    kputs(" -> ");
    kput_dec((uint64_t)id);
    kputs(" (\"");
    kputs(s->name);
    kputs("\") tsc=");
    kput_dec(s->last_active_tsc);
    kputc('\n');
    extern int CHAIN_BLOCK;
    chain_t *cb_block = (CHAIN_BLOCK >= 0) ? chain_get(CHAIN_BLOCK) : 0;
    if (cb_block) cb_block->vault_version++;
    return 0;
}

/* ── Delete ──────────────────────────────────────────────────────── */

int identity_context_delete(int id)
{
    if (id <= 0) return -1;
    id_slot_t *s = slot_by_id(id);
    if (!s) return -1;

    /* Refuse to delete the only context (would lock the user out). */
    int live = 0;
    for (int i = 0; i < IDENTITY_MAX_CONTEXTS; i++)
        if (g_slots[i].used) live++;
    if (live <= 1) {
        kputs("[identity] refuse: cannot delete the only context\n");
        return -1;
    }
    /* Refuse to delete the active context unless caller switched first. */
    if (id == g_active_id) {
        kputs("[identity] refuse: cannot delete the active context\n");
        return -1;
    }

    /* Tear down owner chains best-effort. chain_destroy is idempotent
     * by id; out-of-range ids are no-ops. */
    for (int i = 0; i < s->owner_chains_count; i++) {
        chain_destroy(s->owner_chains[i]);
    }

    /* Zero PIN, release handle. */
    spin_lock(&g_id_lock);
    id_memzero(s->pin_buf, sizeof(s->pin_buf));
    if (s->pin_h) { cfa_release(s->pin_h); s->pin_h = 0; }
    int saved_id = s->id;
    char saved_name[IDENTITY_NAME_MAX];
    id_strncpy(saved_name, s->name, IDENTITY_NAME_MAX);
    id_memzero(s, (int)sizeof(*s));
    spin_unlock(&g_id_lock);

    /* Wipe vault keys. */
    char p[64];
    path_meta(saved_id, p, sizeof(p)); (void)vault_delete(p);
    path_pin (saved_id, p, sizeof(p)); (void)vault_delete(p);
    path_salt(saved_id, p, sizeof(p)); (void)vault_delete(p);
    (void)index_save();

    kputs("[identity] deleted context \"");
    kputs(saved_name);
    kputs("\" id=");
    kput_dec((uint64_t)saved_id);
    kputc('\n');
    return 0;
}

/* ── Vault namespacing ───────────────────────────────────────────── */

int identity_namespace_path(const char *path, char *out, int out_cap)
{
    if (!path || !out || out_cap <= 1) return -1;
    /* Pass through global / pre-init / already-namespaced paths. */
    if (g_active_id == 0
        || (path[0] == '/' && path[1] == 'c' && path[2] == 't' && path[3] == 'x' && path[4] == '/')
        || (path[0] == '/' && path[1] == 'i' && path[2] == 'd' && path[3] == 'e')   /* /identity/ */
        || (path[0] == '/' && path[1] == 'l' && path[2] == 'o' && path[3] == 'c')   /* /lock/ */
        || (path[0] == '/' && path[1] == 'c' && path[2] == 'r' && path[3] == 'y')   /* /crypto/ */
        || (path[0] == '/' && path[1] == 'm' && path[2] == 'a' && path[3] == 's')   /* /masq/ */
        || (path[0] == '/' && path[1] == 'c' && path[2] == 'h' && path[3] == 'a')   /* /chain/ */
        || (path[0] == '/' && path[1] == 't' && path[2] == 'i' && path[3] == 'm'))  /* /time/ */
    {
        int i = 0;
        while (path[i] && i < out_cap - 1) { out[i] = path[i]; i++; }
        out[i] = '\0';
        return i;
    }
    int i = 0;
    const char *p = "/ctx/";
    while (*p && i < out_cap - 1) out[i++] = *p++;
    char num[8]; int n = 0;
    int v = g_active_id; if (v == 0) { num[n++] = '0'; }
    while (v > 0 && n < 7) { num[n++] = '0' + (v % 10); v /= 10; }
    while (n > 0 && i < out_cap - 1) out[i++] = num[--n];
    int j = 0;
    while (path[j] && i < out_cap - 1) out[i++] = path[j++];
    out[i] = '\0';
    return i;
}

/* ── Init ────────────────────────────────────────────────────────── */

/* Loads existing contexts from VAULT. If none exist, migrates the
 * legacy /lock/pin + /crypto/salt pair into context 1 "owner". On
 * first ever boot with no /lock/pin yet, leaves g_active_id = 0 and
 * the lockscreen enrollment path will create the owner context as
 * its final step. */
extern int  lockscreen_pin_copy(char *out, int max);
extern int  lockscreen_pin_configured(void);

void identity_init(void)
{
    if (g_initialized) return;

    for (int i = 0; i < IDENTITY_MAX_CONTEXTS; i++) {
        id_memzero(&g_slots[i], (int)sizeof(g_slots[i]));
    }
    g_active_id = 0;

    vault_mkdir("/identity", VAULT_TIER_INTERNAL);

    uint32_t live_mask = 0;
    uint32_t last_active = 0;
    int have_index = (index_load(&live_mask, &last_active) == 0);

    int loaded = 0;
    if (have_index) {
        for (int bit = 0; bit < 32 && bit < IDENTITY_MAX_CONTEXTS; bit++) {
            if (!(live_mask & (1u << bit))) continue;
            int id = bit + 1;
            int idx = slot_alloc();
            if (idx < 0) break;
            id_slot_t *s = &g_slots[idx];
            /* slot_alloc gave us s with id == something; force to id. */
            s->used = 1; s->id = id;
            if (meta_load(id, s) != 0) {
                /* Corrupt meta for this slot -- drop the slot entry. */
                id_memzero(s, (int)sizeof(*s));
                continue;
            }
            slot_wrap_pin(s);
            loaded++;
        }
    }

    /* Legacy migration: a system that booted under the old single-PIN
     * lockscreen has /lock/pin + /crypto/salt but no /identity index.
     * Create the "owner" context (id=1) using the existing PIN. We
     * don't rekey the disk -- we just point context 1's salt path at
     * the legacy /crypto/salt by writing /identity/1/salt to the same
     * bytes. From here forward the owner uses its own per-context
     * salt and key derivation goes through the identity path. */
    if (loaded == 0 && lockscreen_pin_configured()) {
        char pin[ID_PIN_BUF];
        id_memzero(pin, sizeof(pin));
        int n = lockscreen_pin_copy(pin, sizeof(pin));
        if (n > 0) {
            int id = identity_context_create("owner", pin);
            if (id > 0) {
                /* Copy /crypto/salt -> /identity/1/salt to preserve key. */
                uint8_t saltbuf[32];
                id_memzero(saltbuf, sizeof(saltbuf));
                int got = vault_load_config("/crypto/salt", saltbuf, sizeof(saltbuf));
                if (got == (int)sizeof(saltbuf)) {
                    char sp[64];
                    path_salt(id, sp, sizeof(sp));
                    (void)vault_save_config(sp, saltbuf, sizeof(saltbuf));
                }
                id_memzero(saltbuf, sizeof(saltbuf));
                /* Mark owner active immediately so subsequent chain
                 * creation gets context-rooted CFA. */
                g_active_id = id;
                id_slot_t *s = slot_by_id(id);
                if (s) s->last_active_tsc = timer_read_tsc();
                kputs("[identity] migrated legacy single-PIN -> owner (id=");
                kput_dec((uint64_t)id);
                kputs(")\n");
            }
        }
        id_memzero(pin, sizeof(pin));
    } else if (loaded > 0) {
        /* If the persisted last_active_id is live, mark it active and
         * derive the disk key from its salt. We only do this when we
         * can read the PIN out of the vault and verify it (we can't,
         * pre-PIN-prompt). So instead the COLD-BOOT gate is responsible
         * for prompting and calling identity_context_switch(). For now,
         * leave g_active_id=0 -- shell commands will fail until login. */
        (void)last_active;
    }

    g_initialized = 1;
    kputs("[identity] init: ");
    kput_dec((uint64_t)loaded);
    kputs(" context(s) loaded, active=");
    kput_dec((uint64_t)g_active_id);
    kputc('\n');
}

/* ── Selftest ────────────────────────────────────────────────────── */

void identity_print_selftest_line(void)
{
    int n = 0;
    const char *active_name = "(none)";
    uint32_t root0 = 0;
    for (int i = 0; i < IDENTITY_MAX_CONTEXTS; i++) {
        if (g_slots[i].used) {
            n++;
            if (g_slots[i].id == g_active_id) {
                active_name = g_slots[i].name;
                root0 = g_slots[i].cfa_root_addr.segments[0];
            }
        }
    }
    kputs("  Identity contexts ..... ");
    kput_dec((uint64_t)n);
    kputs(" contexts, active=\"");
    kputs(active_name);
    kputs("\", CFA root 0x");
    kput_hex((uint64_t)root0);
    kputc('\n');
}

/* ── Shell commands ──────────────────────────────────────────────── */

static const char *skip_ws(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static int copy_token(const char *src, char *dst, int cap) {
    int i = 0;
    while (src[i] && src[i] != ' ' && src[i] != '\t' && i < cap - 1) {
        dst[i] = src[i]; i++;
    }
    dst[i] = '\0';
    return i;
}

void cmd_whoami(const char *args)
{
    (void)args;
    if (g_active_id == 0) {
        kputs("  no active context (login required)\n");
        return;
    }
    id_slot_t *s = slot_by_id(g_active_id);
    if (!s) { kputs("  active context invalid\n"); return; }
    kputs("  ");
    kputs(s->name);
    kputs(" (id=");
    kput_dec((uint64_t)s->id);
    kputs(", CFA root 0x");
    kput_hex((uint64_t)s->cfa_root_addr.segments[0]);
    kputs(", chains owned=");
    kput_dec((uint64_t)s->owner_chains_count);
    kputs(")\n");
}

void cmd_contexts(const char *args)
{
    (void)args;
    int any = 0;
    for (int i = 0; i < IDENTITY_MAX_CONTEXTS; i++) {
        if (!g_slots[i].used) continue;
        any = 1;
        kputs("  ");
        kputs(g_slots[i].id == g_active_id ? "*" : " ");
        kputs(" ");
        kputs(g_slots[i].name);
        kputs(" (id=");
        kput_dec((uint64_t)g_slots[i].id);
        kputs(")\n");
    }
    if (!any) kputs("  (no contexts)\n");
}

/* Shell PIN entry: echoed (no terminal raw mode here). The shell
 * commands are intended for non-secret-sensitive use; the gated
 * lockscreen flow is the secure entry point. We accept the PIN as a
 * second token to keep the shell flow non-interactive. */
void cmd_switch_context(const char *args)
{
    args = skip_ws(args);
    if (!*args) {
        kputs("  usage: switch-context <name> <pin>\n");
        return;
    }
    char name[IDENTITY_NAME_MAX];
    int n = copy_token(args, name, sizeof(name));
    if (n == 0) { kputs("  usage: switch-context <name> <pin>\n"); return; }
    args = skip_ws(args + n);
    if (!*args) { kputs("  usage: switch-context <name> <pin>\n"); return; }

    char pin[ID_PIN_BUF];
    (void)copy_token(args, pin, sizeof(pin));

    int id = identity_context_by_name(name);
    if (id < 0) { kputs("  unknown context\n"); id_memzero(pin, sizeof(pin)); return; }

    int rc = identity_context_switch(id, pin);
    id_memzero(pin, sizeof(pin));
    if (rc != 0) { kputs("  switch failed\n"); return; }
    kputs("  switched to ");
    kputs(name);
    kputc('\n');
}

void cmd_new_context(const char *args)
{
    args = skip_ws(args);
    if (!*args) {
        kputs("  usage: new-context <name> <pin>\n");
        return;
    }
    char name[IDENTITY_NAME_MAX];
    int n = copy_token(args, name, sizeof(name));
    if (n == 0) { kputs("  usage: new-context <name> <pin>\n"); return; }
    args = skip_ws(args + n);
    if (!*args) { kputs("  usage: new-context <name> <pin>\n"); return; }

    char pin[ID_PIN_BUF];
    (void)copy_token(args, pin, sizeof(pin));

    int id = identity_context_create(name, pin);
    id_memzero(pin, sizeof(pin));
    if (id < 0) { kputs("  create failed\n"); return; }
    kputs("  created context \""); kputs(name);
    kputs("\" id="); kput_dec((uint64_t)id); kputc('\n');
}

void cmd_delete_context(const char *args)
{
    args = skip_ws(args);
    if (!*args) {
        kputs("  usage: delete-context <name>\n");
        return;
    }
    char name[IDENTITY_NAME_MAX];
    (void)copy_token(args, name, sizeof(name));
    int id = identity_context_by_name(name);
    if (id < 0) { kputs("  unknown context\n"); return; }
    int rc = identity_context_delete(id);
    if (rc != 0) { kputs("  delete failed\n"); return; }
    kputs("  deleted ");
    kputs(name);
    kputc('\n');
}
