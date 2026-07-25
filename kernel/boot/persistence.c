/*
 * Zeos -- Persistence Layer (implementation)
 *
 * Sinks the masq_journal ring + chain registry tunables/B3 history to
 * VAULT keys so reboots don't wipe provenance. See persistence.h for
 * the contract.
 *
 * Boot order (main.c):
 *   block_init()
 *   vault_persist_init() / vault_init_ramdisk()  <- mounts VAULT,
 *                                                   loads from disk
 *                                                   if backing exists
 *   persistence_init()                           <- replays journal
 *                                                   into RAM ring,
 *                                                   buffers snapshot
 *   chain_registry_init()                        <- creates chains
 *                                                   then calls
 *                                                   persistence_apply_snapshot()
 *
 * Hot path:
 *   block_chain.c journal_append() -> persistence_journal_append()
 *   chain.c       chain_resolve()  -> persistence_on_resolve_complete()
 */

#include "persistence.h"
#include "timeofday.h"
#include "vault.h"
#include "chain.h"
#include "kprint.h"
#include "timer.h"
#include "spinlock.h"

#include <stdint.h>

/* Coarse lock for snapshot save + journal-append-to-VAULT. Idempotent
 * flush — multiple cores arriving at the checkpoint counter at once
 * collapse to one save. */
static zeos_spinlock_t g_persist_lock = ZEOS_SPINLOCK_INIT;

/* ── Internal helpers (freestanding) ────────────────────────────── */

/* Standard CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320). Used to
 * gate whole-snapshot acceptance — a single bit-flip anywhere in the
 * snapshot payload trips the CRC and the snapshot is discarded.
 *
 * Implemented byte-at-a-time; this runs at most once per boot, on a
 * buffer bounded by the static snapshot scratch. No table to keep the
 * code surface small. */
static uint32_t p_crc32(const void *data, uint32_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        crc ^= (uint32_t)p[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = -(int32_t)(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

/* True if `f` is a finite, non-NaN, non-Inf, non-negative float in a
 * sane range for B3 priors. Used to reject corrupt records without
 * pulling <math.h>. We avoid touching the float in any way that would
 * fault on a NaN payload. */
static int p_finite_nonneg(float f)
{
    union { float f; uint32_t u; } cv;
    cv.f = f;
    uint32_t exp  = (cv.u >> 23) & 0xFFu;
    uint32_t sign = (cv.u >> 31) & 0x1u;
    if (sign != 0u)        return 0;  /* negative */
    if (exp  == 0xFFu)     return 0;  /* Inf or NaN */
    return 1;
}

static void p_memcpy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}

static void p_memset(void *dst, uint8_t v, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    for (uint32_t i = 0; i < n; i++) d[i] = v;
}

static int p_streq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

static void p_strncpy(char *dst, const char *src, uint32_t cap)
{
    uint32_t i = 0;
    if (cap == 0) return;
    while (i < cap - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* ── Module state ───────────────────────────────────────────────── */

static int      g_ready = 0;

/* Restore stats (set during persistence_init). */
static uint32_t g_journal_restored;
static uint32_t g_chains_restored;

/* Save stats. */
static uint32_t g_snapshot_saves;
static uint64_t g_last_save_tsc;

/* Resolve counter -> drives the checkpoint cadence. */
static uint64_t g_resolve_count;
#define CHECKPOINT_EVERY 500ULL

/* Deferred-checkpoint flag (A.9 fix, 2026-07-25). The checkpoint used to call
 * persistence_save_snapshot_now() DIRECTLY from persistence_on_resolve_complete,
 * which runs in chain_resolve()'s epilogue -- so the snapshot's vault_sync ->
 * vault_persist_flush -> block_chain_submit -> chain_resolve(CHAIN_BLOCK) was a
 * RE-ENTRANT chain_resolve from inside a chain_resolve, and it wedged the whole
 * scheduler hard the first time the 500-resolve threshold tripped (~tick 4).
 * Root-caused by bisection: skipping the save let the scheduler run to tick
 * 2880+ cleanly. Fix: the hot-path hook only SETS this flag; the actual disk
 * flush runs from the scheduler loop at top level (persistence_checkpoint_if_due),
 * outside any chain_resolve -- the same context CHAIN_BLOCK is normally driven
 * from, so no re-entrancy and no heavy blocking I/O on the per-resolve path. */
static volatile int g_snapshot_due;

/* Snapshot of records loaded at boot, applied later by
 * persistence_apply_snapshot() once chain_registry_init has built the
 * registry. Sized to MAX_CHAINS to match chain.h. */
static persistence_chain_record_t g_pending_snapshot[128];
static uint32_t                   g_pending_count;

/* ── Forward decls for block_chain replay hook ───────────────────── */

/* In block_chain.c -- inserts a record directly into the in-RAM ring
 * without re-touching VAULT or bumping vault_version. Used only during
 * boot-time replay. */
extern void block_chain_journal_replay_record(uint64_t tsc, int drive_id,
                                              uint64_t lba, uint32_t count,
                                              int op, int prior_vv, int new_vv);

/* ── Journal load (boot replay) ─────────────────────────────────── */

static void load_journal_into_ring(void)
{
    int sz = vault_size(PERSISTENCE_JOURNAL_PATH);
    if (sz <= 0) {
        g_journal_restored = 0;
        return;
    }

    /* Read the whole file. We cap reads at ~256 KiB to keep the stack
     * buffer bounded; PERSISTENCE_JOURNAL_CAP * sizeof(record) is
     * around 320 KiB so for now we read in chunks of ~64 KiB. */
    static uint8_t scratch[64 * 1024];
    uint32_t total = (uint32_t)sz;
    uint32_t recs  = total / (uint32_t)sizeof(persistence_journal_record_t);
    if (recs == 0) {
        g_journal_restored = 0;
        return;
    }

    /* vault_read returns the file from offset 0, capped at scratch.
     * For files larger than scratch we'd need an offset variant; for
     * now we read up to scratch bytes (worst case ~1638 records). */
    uint32_t to_read = total;
    if (to_read > sizeof(scratch)) to_read = sizeof(scratch);

    int got = vault_read(PERSISTENCE_JOURNAL_PATH, scratch, to_read);
    if (got <= 0) {
        g_journal_restored = 0;
        return;
    }

    uint32_t got_recs = (uint32_t)got / (uint32_t)sizeof(persistence_journal_record_t);
    persistence_journal_record_t *arr = (persistence_journal_record_t *)scratch;

    /* Replay newest 256 (BLOCK_JOURNAL_CAP) into the in-RAM ring so
     * `masq-journal` shows pre-reboot writes. */
    uint32_t start = 0;
    if (got_recs > 256) start = got_recs - 256;
    for (uint32_t i = start; i < got_recs; i++) {
        block_chain_journal_replay_record(arr[i].tsc,
                                          arr[i].drive_id,
                                          arr[i].lba,
                                          arr[i].count,
                                          arr[i].op,
                                          arr[i].prior_vault_version,
                                          arr[i].new_vault_version);
    }
    g_journal_restored = got_recs;
}

/* ── Snapshot load (boot, deferred apply) ───────────────────────── */

/* Validate one chain_record_t. Returns 1 if usable, 0 if corrupt.
 *
 * A corrupt record is the signature of a stale snapshot mismatching
 * the current chain registry shape (renamed slot, reordered enum,
 * partial overwrite from an unrelated VAULT key). We treat any
 * out-of-range field as fatal for that record — applying it would
 * write garbage into a live chain's tunables and could feed bad B3
 * priors into the scheduler's backoff math, which has been observed
 * to send execution through stale code addresses on the next resolve.
 */
static int validate_chain_record(const persistence_chain_record_t *r)
{
    /* name must be NUL-terminated within the buffer. */
    int has_nul = 0;
    for (uint32_t i = 0; i < sizeof(r->name); i++) {
        if (r->name[i] == '\0') { has_nul = 1; break; }
    }
    if (!has_nul) return 0;
    if (r->name[0] == '\0') return 0;  /* empty name slot */

    /* parent_id must be -1 (root) or a valid registry index. */
    if (r->parent_id != -1 && (r->parent_id < 0 || r->parent_id >= 128))
        return 0;

    /* tier must be a valid enum. */
    if (r->tier > (uint8_t)MASQ_REFERENCE) return 0;

    /* B3 priors must be finite, non-negative. NaN/Inf/negative would
     * make the scheduler's backoff_skip_threshold compare nonsensical. */
    if (!p_finite_nonneg(r->b3_alpha))               return 0;
    if (!p_finite_nonneg(r->b3_beta))                return 0;
    if (!p_finite_nonneg(r->backoff_skip_threshold)) return 0;

    if (r->b3_observations < 0)  return 0;
    if (r->vault_version  < 0)   return 0;

    return 1;
}

static void load_snapshot_buffered(void)
{
    g_pending_count = 0;
    g_chains_restored = 0;

    int sz = vault_size(PERSISTENCE_SNAPSHOT_PATH);
    if (sz < (int)sizeof(persistence_snapshot_header_t)) return;

    static uint8_t buf[sizeof(persistence_snapshot_header_t) +
                       128 * sizeof(persistence_chain_record_t)];
    uint32_t to_read = (uint32_t)sz;
    if (to_read > sizeof(buf)) to_read = sizeof(buf);

    int got = vault_read(PERSISTENCE_SNAPSHOT_PATH, buf, to_read);
    if (got < (int)sizeof(persistence_snapshot_header_t)) return;

    persistence_snapshot_header_t *hdr = (persistence_snapshot_header_t *)buf;
    if (hdr->magic != PERSISTENCE_SNAPSHOT_MAGIC) {
        kputs("[persistence] snapshot magic mismatch -- discarding\n");
        return;
    }
    if (hdr->version != PERSISTENCE_SNAPSHOT_VERSION) {
        kputs("[persistence] snapshot schema v");
        kput_dec((uint64_t)hdr->version);
        kputs(" != current v");
        kput_dec((uint64_t)PERSISTENCE_SNAPSHOT_VERSION);
        kputs(" -- using fresh state\n");
        return;
    }

    /* Sanity-cap chain_count BEFORE we use it for size math, so a
     * corrupt header with chain_count=0xFFFFFFFF can't wrap the
     * `expected` multiplication and slip a too-small buffer past the
     * size check. */
    if (hdr->chain_count > 128) {
        kputs("[persistence] snapshot chain_count out of range -- discarding\n");
        return;
    }

    uint32_t expected = sizeof(persistence_snapshot_header_t)
                      + hdr->chain_count * (uint32_t)sizeof(persistence_chain_record_t);
    if ((uint32_t)got < expected) {
        kputs("[persistence] snapshot truncated -- discarding\n");
        return;
    }

    /* CRC covers everything after the magic/version/crc32/reserved0
     * prefix: saved_tsc, chain_count, reserved1, and the record array. */
    uint32_t prefix = (uint32_t)((uint8_t *)&hdr->saved_tsc - buf);
    uint32_t crc_expected = hdr->crc32;
    uint32_t crc_got      = p_crc32(buf + prefix, expected - prefix);
    if (crc_got != crc_expected) {
        kputs("[persistence] snapshot CRC mismatch -- discarding (stale or corrupt)\n");
        return;
    }

    persistence_chain_record_t *src =
        (persistence_chain_record_t *)(buf + sizeof(persistence_snapshot_header_t));

    /* Per-record validation. We accept the snapshot as a whole, then
     * skip individual records that fail validation. A pile of failed
     * records is logged once. */
    uint32_t kept = 0;
    uint32_t dropped = 0;
    for (uint32_t i = 0; i < hdr->chain_count; i++) {
        if (validate_chain_record(&src[i])) {
            g_pending_snapshot[kept++] = src[i];
        } else {
            dropped++;
        }
    }
    if (dropped) {
        kputs("[persistence] dropped ");
        kput_dec((uint64_t)dropped);
        kputs(" corrupt snapshot record(s)\n");
    }
    g_pending_count = kept;
}

/* ── Public init ────────────────────────────────────────────────── */

void persistence_init(void)
{
    if (g_ready) return;

    g_journal_restored      = 0;
    g_chains_restored       = 0;
    g_snapshot_saves        = 0;
    g_last_save_tsc         = 0;
    g_resolve_count         = 0;
    g_pending_count         = 0;

    /* Ensure the directories exist for first-boot writes. The mkdir
     * is idempotent under VAULT (returns -1 if already present, which
     * we ignore). */
    vault_mkdir("/masq",  VAULT_TIER_INTERNAL);
    vault_mkdir("/chain", VAULT_TIER_INTERNAL);

    load_journal_into_ring();
    load_snapshot_buffered();

    /* Time-of-day persistence: if CMOS read failed in tod_init(), try
     * to restore the last-known wall clock from VAULT so logs aren't
     * stamped 1970. No-op if tod is already CMOS-backed. */
    tod_persist_load();

    g_ready = 1;

    kputs("[persistence] init: journal=");
    kput_dec((uint64_t)g_journal_restored);
    kputs(" entries, snapshot=");
    kput_dec((uint64_t)g_pending_count);
    kputs(" chain records pending\n");
}

/* ── Apply snapshot to the live registry ─────────────────────────── */

int persistence_apply_snapshot(void)
{
    if (!g_ready || g_pending_count == 0) {
        g_chains_restored = 0;
        return 0;
    }

    uint32_t applied = 0;
    uint32_t shape_skipped = 0;
    for (uint32_t i = 0; i < g_pending_count; i++) {
        persistence_chain_record_t *rec = &g_pending_snapshot[i];

        /* Match by name across the registry. Walk all 128 slots; the
         * registry is sparse but the upper bound is fixed. */
        for (int id = 0; id < 128; id++) {
            chain_t *c = chain_get(id);
            if (!c) continue;
            if (!p_streq(c->name, rec->name)) continue;

            /* Shape consistency: a snapshot whose persisted parent_id
             * disagrees with the live chain's parent_id describes a
             * different topology — likely a renamed/relocated chain
             * across kernel revisions. Skip rather than write tunables
             * onto a same-name-but-different chain. parent_id == -1
             * (root) is permissive: we don't reject root mismatches
             * because chain_registry_init may have re-rooted some
             * chains in-tree without the persisted record knowing. */
            if (rec->parent_id != -1 && rec->parent_id != c->parent_id) {
                shape_skipped++;
                break;
            }

            /* Restore mutable state. NEVER touch resolve fn pointers,
             * node count, parent_id, addr, last_resolve_* (transient).
             *
             * tier is also persisted but only restored if it matches
             * the live chain's current tier; a tier downgrade in code
             * (e.g. SOVEREIGN -> INTERNAL) must not be undone by the
             * snapshot, which would re-elevate the chain's MasQ scope
             * silently. */
            if ((uint8_t)c->tier == rec->tier) {
                c->tier = (masq_tier_t)rec->tier;
            }
            c->b3_alpha               = rec->b3_alpha;
            c->b3_beta                = rec->b3_beta;
            c->b3_observations        = rec->b3_observations;
            c->vault_version          = rec->vault_version;
            c->watchdog_timeout_us    = rec->watchdog_timeout_us;
            c->backoff_skip_threshold = rec->backoff_skip_threshold;
            c->backoff_skip_every     = rec->backoff_skip_every;
            applied++;
            break;
        }
    }
    if (shape_skipped) {
        kputs("[persistence] skipped ");
        kput_dec((uint64_t)shape_skipped);
        kputs(" snapshot record(s) due to topology mismatch\n");
    }
    g_chains_restored = applied;

    kputs("[chain_registry] restored ");
    kput_dec((uint64_t)applied);
    kputs(" chains from VAULT snapshot\n");
    return (int)applied;
}

/* ── Journal append (hot path) ──────────────────────────────────── */

void persistence_journal_append(uint64_t tsc, int drive_id, uint64_t lba,
                                uint32_t count, int op,
                                int prior_vv, int new_vv)
{
    if (!g_ready) return;

    persistence_journal_record_t rec;
    p_memset(&rec, 0, sizeof(rec));
    rec.tsc                 = tsc;
    rec.drive_id            = drive_id;
    rec.lba                 = lba;
    rec.count               = count;
    rec.op                  = op;
    rec.prior_vault_version = prior_vv;
    rec.new_vault_version   = new_vv;

    spin_lock(&g_persist_lock);
    /* vault_append creates the file on first call. The append-mode
     * path inside VAULT extends the existing inode without minting a
     * new temporal version, which is what we want for a log. */
    (void)vault_append(PERSISTENCE_JOURNAL_PATH, &rec, sizeof(rec));
    spin_unlock(&g_persist_lock);

    /* Eviction: VAULT_DIRECT_BLOCKS * VAULT_BLOCK_SIZE = 12 * 4096 =
     * 48 KiB cap on a single inode in the current VAULT impl. That
     * caps us at ~1228 records, far below PERSISTENCE_JOURNAL_CAP.
     * When append fails (file full) we'd ideally roll the file --
     * for now the cap is whatever the inode can hold; the in-RAM
     * ring is unaffected and provenance for the most-recent 256
     * records is always live. A future change can rotate the file
     * once an indirect-block path lands in vault.c. */
}

/* ── Snapshot save (checkpoint) ─────────────────────────────────── */

int persistence_save_snapshot_now(void)
{
    if (!g_ready) return -1;
    spin_lock(&g_persist_lock);

    /* Build the snapshot in a stack buffer. */
    static uint8_t buf[sizeof(persistence_snapshot_header_t) +
                       128 * sizeof(persistence_chain_record_t)];
    p_memset(buf, 0, sizeof(buf));

    persistence_snapshot_header_t *hdr = (persistence_snapshot_header_t *)buf;
    hdr->magic     = PERSISTENCE_SNAPSHOT_MAGIC;
    hdr->version   = PERSISTENCE_SNAPSHOT_VERSION;
    hdr->crc32     = 0;  /* placeholder, computed once payload is built */
    hdr->reserved0 = 0;
    hdr->saved_tsc = timer_read_tsc();

    persistence_chain_record_t *records =
        (persistence_chain_record_t *)(buf + sizeof(persistence_snapshot_header_t));

    uint32_t n = 0;
    for (int id = 0; id < 128 && n < 128; id++) {
        chain_t *c = chain_get(id);
        if (!c) continue;

        persistence_chain_record_t *r = &records[n];
        p_memset(r, 0, sizeof(*r));
        p_strncpy(r->name, c->name, sizeof(r->name));
        r->parent_id              = c->parent_id;
        r->tier                   = (uint8_t)c->tier;
        r->b3_alpha               = c->b3_alpha;
        r->b3_beta                = c->b3_beta;
        r->b3_observations        = c->b3_observations;
        r->vault_version          = c->vault_version;
        r->watchdog_timeout_us    = c->watchdog_timeout_us;
        r->backoff_skip_threshold = c->backoff_skip_threshold;
        r->backoff_skip_every     = c->backoff_skip_every;
        n++;
    }
    hdr->chain_count = n;

    uint32_t total = sizeof(persistence_snapshot_header_t)
                   + n * (uint32_t)sizeof(persistence_chain_record_t);

    /* CRC over everything after magic/version/crc32/reserved0. The
     * loader recomputes this and discards the snapshot on mismatch. */
    uint32_t prefix = (uint32_t)((uint8_t *)&hdr->saved_tsc - buf);
    hdr->crc32 = p_crc32(buf + prefix, total - prefix);

    /* vault_write replaces the file (creates a new temporal version
     * if it already exists) -- exactly what we want for a single
     * rolling snapshot key. */
    int wrote = vault_write(PERSISTENCE_SNAPSHOT_PATH, buf, total);
    if (wrote < 0) {
        spin_unlock(&g_persist_lock);
        return -1;
    }

    /* Push the dirty RAM image down to the persistent backing drive. */
    vault_sync();

    g_snapshot_saves++;
    g_last_save_tsc = hdr->saved_tsc;

    /* Sink the current wall clock too. */
    tod_persist_save();
    spin_unlock(&g_persist_lock);
    return 0;
}

/* ── Resolve checkpoint hook ─────────────────────────────────────── */

void persistence_on_resolve_complete(void)
{
    if (!g_ready) return;

    /* Atomic increment + threshold check; only one CPU's increment
     * crosses the boundary so save runs exactly once per CHECKPOINT_EVERY
     * resolves regardless of which core hit the boundary. */
    uint64_t v = (uint64_t)__sync_add_and_fetch(&g_resolve_count, 1);
    if ((v % CHECKPOINT_EVERY) == 0) {
        /* Defer, don't flush here (A.9): this runs inside chain_resolve()'s
         * epilogue; doing the disk flush now re-enters chain_resolve via the
         * block chain and wedges the scheduler. Just mark it due. */
        g_snapshot_due = 1;
    }
}

/* Run a deferred checkpoint if one is due. MUST be called from the scheduler
 * loop at TOP LEVEL -- never from inside a chain_resolve (that's the re-entrancy
 * the deferral exists to avoid). Cheap no-op when nothing is due. */
void persistence_checkpoint_if_due(void)
{
    if (!g_ready) return;
    if (!__sync_bool_compare_and_swap(&g_snapshot_due, 1, 0)) return;
    (void)persistence_save_snapshot_now();
}

/* ── Test hook: pure validation pipeline over caller-provided buffer ──
 *
 * Mirrors the structural checks in load_snapshot_buffered() but with
 * NO side effects on module state — no g_pending_count touch, no log
 * output of "loaded N records" claims. Used by the boot regression
 * test in tests.c to prove that a malformed snapshot is rejected
 * gracefully, not by triggering a fault.
 */
int persistence_validate_snapshot_buffer(const void *buf, uint32_t len)
{
    if (!buf) return 0;
    if (len < sizeof(persistence_snapshot_header_t)) return 0;

    const persistence_snapshot_header_t *hdr =
        (const persistence_snapshot_header_t *)buf;

    if (hdr->magic    != PERSISTENCE_SNAPSHOT_MAGIC)      return 0;
    if (hdr->version  != PERSISTENCE_SNAPSHOT_VERSION)    return 0;
    if (hdr->chain_count > 128)                           return 0;

    uint32_t expected = sizeof(persistence_snapshot_header_t)
                      + hdr->chain_count * (uint32_t)sizeof(persistence_chain_record_t);
    if (len < expected) return 0;

    uint32_t prefix = (uint32_t)((const uint8_t *)&hdr->saved_tsc - (const uint8_t *)buf);
    uint32_t crc_got =
        p_crc32((const uint8_t *)buf + prefix, expected - prefix);
    if (crc_got != hdr->crc32) return 0;

    const persistence_chain_record_t *src =
        (const persistence_chain_record_t *)((const uint8_t *)buf
                                             + sizeof(persistence_snapshot_header_t));
    int kept = 0;
    for (uint32_t i = 0; i < hdr->chain_count; i++) {
        if (validate_chain_record(&src[i])) kept++;
    }
    return kept;
}

/* ── Accessors ───────────────────────────────────────────────────── */

uint32_t persistence_journal_restored_count(void)  { return g_journal_restored; }
uint32_t persistence_chains_restored_count(void)   { return g_chains_restored; }
uint32_t persistence_snapshot_save_count(void)     { return g_snapshot_saves; }
uint64_t persistence_last_save_tsc(void)           { return g_last_save_tsc; }
uint64_t persistence_resolve_count(void)           { return g_resolve_count; }
int      persistence_ready(void)                   { return g_ready; }

/* ── Dump (shell `persistence` command) ──────────────────────────── */

void persistence_dump(void)
{
    kputs("─ persistence ─\n");
    kputs("  ready: ");
    kputs(g_ready ? "yes" : "no");
    kputc('\n');

    /* Journal file */
    kputs("  ");
    kputs(PERSISTENCE_JOURNAL_PATH);
    kputs(": ");
    int jsz = vault_size(PERSISTENCE_JOURNAL_PATH);
    if (jsz < 0) {
        kputs("(missing)\n");
    } else {
        kput_dec((uint64_t)jsz);
        kputs(" bytes (");
        kput_dec((uint64_t)((uint32_t)jsz / sizeof(persistence_journal_record_t)));
        kputs(" records)\n");
    }

    /* Snapshot file */
    kputs("  ");
    kputs(PERSISTENCE_SNAPSHOT_PATH);
    kputs(": ");
    int ssz = vault_size(PERSISTENCE_SNAPSHOT_PATH);
    if (ssz < 0) {
        kputs("(missing)\n");
    } else {
        kput_dec((uint64_t)ssz);
        kputs(" bytes\n");
    }

    /* Counters */
    kputs("  journal_restored_at_boot: ");
    kput_dec((uint64_t)g_journal_restored);
    kputc('\n');
    kputs("  chains_restored_at_boot:  ");
    kput_dec((uint64_t)g_chains_restored);
    kputc('\n');
    kputs("  snapshots_saved_this_boot: ");
    kput_dec((uint64_t)g_snapshot_saves);
    kputc('\n');
    kputs("  last_save_tsc: ");
    kput_dec(g_last_save_tsc);
    kputc('\n');
    kputs("  resolves_observed: ");
    kput_dec(g_resolve_count);
    kputs(" (checkpoint every ");
    kput_dec(CHECKPOINT_EVERY);
    kputs(")\n");
}
