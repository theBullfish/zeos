/*
 * Zeos -- Persistence Layer
 *
 * Two responsibilities:
 *   1. masq_journal sink -- mirror the in-RAM ring of block-write
 *      provenance records to VAULT so they survive a reboot.
 *   2. Chain registry snapshot -- periodically (every checkpoint
 *      interval of resolves) serialize the live chain registry's
 *      mutable state (B3 priors, vault_version, watchdog/backoff
 *      tunables) to a single VAULT key so the next boot can rebind
 *      the rebuilt chains to their learned values.
 *
 * Function pointer state and last_resolve_* are NEVER persisted --
 * the kernel rebuilds those on every boot.
 */

#ifndef ZEOS_PERSISTENCE_H
#define ZEOS_PERSISTENCE_H

#include <stdint.h>

/* On-disk schema version for the chain registry snapshot. Bump when
 * the persisted chain shape changes; older snapshots are discarded.
 *
 * v2 (2026-05-04): added crc32 to header + per-record sanity gating in
 * load_snapshot_buffered() and parent_id consistency check in
 * persistence_apply_snapshot(). Any v1 snapshot in an existing vault is
 * discarded on first v2 boot — see boot fix in commit history.
 */
#define PERSISTENCE_SNAPSHOT_VERSION  2

/* On-disk schema version for individual journal records. */
#define PERSISTENCE_JOURNAL_VERSION   1

/* Cap on persisted journal entries. Older records are evicted as a
 * sliding tail when the file exceeds this many records. The in-RAM
 * ring stays at its 256-entry default (block_chain.c). */
#define PERSISTENCE_JOURNAL_CAP       8192

/* VAULT keys (paths) used by this layer. */
#define PERSISTENCE_JOURNAL_PATH      "/masq/journal.bin"
#define PERSISTENCE_SNAPSHOT_PATH     "/chain/registry.snapshot"

/* Persisted-on-disk journal record. Mirrors the in-RAM journal_rec_t
 * in block_chain.c but is wire-stable. */
typedef struct {
    uint64_t tsc;
    uint64_t lba;
    uint32_t count;
    int32_t  drive_id;
    int32_t  op;
    int32_t  prior_vault_version;
    int32_t  new_vault_version;
} persistence_journal_record_t;

/* Snapshot file header. Followed by snapshot_count chain_record_t.
 *
 * crc32 covers all bytes from the start of `saved_tsc` through the end
 * of the chain_record_t array (i.e. everything after the header's
 * magic/version/crc32 prefix). Computed with the standard IEEE 802.3
 * polynomial (0xEDB88320, reflected). Mismatch => snapshot discarded. */
typedef struct {
    uint32_t magic;             /* 'ZCRS' = 0x5A435253 */
    uint32_t version;           /* PERSISTENCE_SNAPSHOT_VERSION */
    uint32_t crc32;             /* covers everything after this field */
    uint32_t reserved0;         /* keeps header 8-byte aligned for saved_tsc */
    uint64_t saved_tsc;
    uint32_t chain_count;
    uint32_t reserved1;
} persistence_snapshot_header_t;

#define PERSISTENCE_SNAPSHOT_MAGIC    0x5A435253u

typedef struct {
    char     name[64];
    int32_t  parent_id;         /* registry slot id at save time (advisory) */
    uint8_t  tier;
    uint8_t  _pad0[3];
    float    b3_alpha;
    float    b3_beta;
    int32_t  b3_observations;
    int32_t  vault_version;
    uint32_t watchdog_timeout_us;
    float    backoff_skip_threshold;
    uint32_t backoff_skip_every;
    uint32_t reserved[4];
} persistence_chain_record_t;

/* Initialize the persistence layer.
 * Must be called AFTER vault is mounted and BEFORE chain_registry_init
 * starts creating chains (so journal-replay results are visible to the
 * shell before any new writes accumulate).
 *
 * On entry: reads the journal file from VAULT and replays it into the
 * block_chain in-RAM ring (newest-last). Reads the chain snapshot file
 * into a private buffer for later application via
 * persistence_apply_snapshot().
 */
void persistence_init(void);

/* Apply the previously-loaded snapshot to the live chain registry.
 * Matches by chain name; no-op for chains that don't have a record.
 * Returns the number of chains restored. */
int  persistence_apply_snapshot(void);

/* Append one record to the persisted journal file. Called from
 * block_chain.c journal_append() under the same write that updates the
 * in-RAM ring. Trims the on-disk file to PERSISTENCE_JOURNAL_CAP
 * records when it grows past the cap. */
void persistence_journal_append(uint64_t tsc, int drive_id, uint64_t lba,
                                uint32_t count, int op,
                                int prior_vv, int new_vv);

/* Resolve-completion checkpoint hook. Called from chain_resolve() at
 * the tail of every successful resolve. Once enough resolves have
 * accumulated since the last checkpoint, marks a snapshot DUE (does not
 * flush here -- that would re-enter chain_resolve; see A.9). Cheap: a
 * counter compare + flag set on the hot path. */
void persistence_on_resolve_complete(void);

/* Run a deferred checkpoint if one is due. Call from the scheduler loop at
 * TOP LEVEL only (never inside a chain_resolve). No-op when none is due. */
void persistence_checkpoint_if_due(void);

/* Force an immediate snapshot. Returns 0 on success, -1 on error. */
int  persistence_save_snapshot_now(void);

/* Counters for the selftest + 'persistence' shell command. */
uint32_t persistence_journal_restored_count(void);
uint32_t persistence_chains_restored_count(void);
uint32_t persistence_snapshot_save_count(void);
uint64_t persistence_last_save_tsc(void);
uint64_t persistence_resolve_count(void);

/* True if persistence has been initialized this boot. */
int      persistence_ready(void);

/* Print the on-disk journal + snapshot stats. Used by the
 * 'persistence' shell command. */
void     persistence_dump(void);

/* ── Test hook ────────────────────────────────────────────────────────
 * Runs the snapshot loader's validation pipeline on a caller-provided
 * buffer (header + records, just like the on-disk layout). Used by the
 * boot regression test to confirm that:
 *   - a well-formed v2 snapshot loads cleanly,
 *   - a corrupt header / mismatched CRC / out-of-range record is
 *     rejected without panicking.
 *
 * Return: count of records that passed validation (0 if rejected as a
 * whole snapshot, e.g. magic/version/CRC mismatch). Never panics on
 * malformed input — that's the whole point.
 *
 * Note: does NOT touch g_pending_snapshot or any live state. Pure
 * validation, side-effect-free.
 */
int persistence_validate_snapshot_buffer(const void *buf, uint32_t len);

#endif /* ZEOS_PERSISTENCE_H */
