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
        kputs(" -- discarding\n");
        return;
    }

    uint32_t expected = sizeof(persistence_snapshot_header_t)
                      + hdr->chain_count * (uint32_t)sizeof(persistence_chain_record_t);
    if ((uint32_t)got < expected) return;

    persistence_chain_record_t *src =
        (persistence_chain_record_t *)(buf + sizeof(persistence_snapshot_header_t));

    uint32_t n = hdr->chain_count;
    if (n > 128) n = 128;

    for (uint32_t i = 0; i < n; i++) g_pending_snapshot[i] = src[i];
    g_pending_count = n;
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
    for (uint32_t i = 0; i < g_pending_count; i++) {
        persistence_chain_record_t *rec = &g_pending_snapshot[i];

        /* Match by name across the registry. Walk all 128 slots; the
         * registry is sparse but the upper bound is fixed. */
        for (int id = 0; id < 128; id++) {
            chain_t *c = chain_get(id);
            if (!c) continue;
            if (!p_streq(c->name, rec->name)) continue;

            /* Restore mutable state. NEVER touch resolve fn pointers,
             * node count, parent_id, addr, last_resolve_* (transient). */
            c->b3_alpha               = rec->b3_alpha;
            c->b3_beta                = rec->b3_beta;
            c->b3_observations        = rec->b3_observations;
            c->vault_version          = rec->vault_version;
            c->tier                   = (masq_tier_t)rec->tier;
            c->watchdog_timeout_us    = rec->watchdog_timeout_us;
            c->backoff_skip_threshold = rec->backoff_skip_threshold;
            c->backoff_skip_every     = rec->backoff_skip_every;
            applied++;
            break;
        }
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
        (void)persistence_save_snapshot_now();
    }
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
