/*
 * Zeos -- Chain-native block I/O
 *
 * Chain-native block I/O. Pipeline:
 *   block_request -> addressing -> masq_journal -> hardware_dma
 * NVMe, AHCI, USB-MSC drivers are the hardware_dma backend.
 * masq_journal records every write with prior vault_version for
 * temporal provenance. Reads do not bump vault_version.
 * MasQ tier: INTERNAL. Parent: CHAIN_CPU.
 *
 * Per the contract: state-changing resolves bump chain->vault_version,
 * idle/read paths do not. The journal is the load-bearing addition --
 * every write carries a who/what/when/why-prior-state record before
 * the bytes ever reach hardware. The compat shims in block.c forward
 * legacy block_read/block_write through this chain so all existing
 * callers (FAT32 mount, lsdrives, installer, vault_disk) become chain
 * users without a single edit.
 */

#include "block_chain.h"
#include "block.h"
#include "chain.h"
#include "chain_registry.h"
#include "nvme.h"
#include "ahci.h"
#include "usb_msc.h"
#include "kprint.h"
#include "timer.h"
#include "persistence.h"
#include <stdint.h>

/* ── Public chain ID ───────────────────────────────────────────── */

int CHAIN_BLOCK = -1;

/* ── In-flight request slot ────────────────────────────────────── */
/*
 * One request in flight at a time, mirroring the polling-only nature
 * of every storage driver in this tree (no IRQ completion path).
 * The request struct is module-private; the chain pipeline reads it
 * from the first node and the result is reflected back in the slot.
 */
typedef struct {
    int      valid;
    int      drive_id;
    uint64_t lba;
    uint32_t count;
    int      op;            /* BLOCK_OP_READ / WRITE / FLUSH */
    void    *buf;
    /* Filled by addressing_resolve from block_drive_info. */
    int      kind;
    int      sub_idx;
    uint32_t sector_size;
    uint64_t sectors_total;
    int      error;
    /* Filled by masq_journal_resolve. */
    int      journaled;
    int      prior_vault_version;
    /* Filled by hardware_dma_resolve. */
    int      hw_rc;
} block_chain_request_t;

static block_chain_request_t s_req;

/* The two stage payloads carried between nodes. We use pointers into
 * s_req rather than copying data through the 4 KiB scratch buffers --
 * the buffers can't carry a sector buffer pointer + metadata cleanly,
 * and the request lives for the whole chain_resolve() call. */
typedef struct {
    block_chain_request_t *req;
} block_stage_t;

/* ── Journal ring buffer ───────────────────────────────────────── */

#define BLOCK_JOURNAL_CAP 256

typedef struct {
    uint64_t tsc;                   /* TSC at journal time */
    uint64_t lba;
    uint32_t count;
    int      drive_id;
    int      op;                    /* WRITE / FLUSH (reads not journaled) */
    int      prior_vault_version;
    int      new_vault_version;
} journal_rec_t;

static journal_rec_t s_journal[BLOCK_JOURNAL_CAP];
static uint64_t      s_journal_total;     /* monotonic; index = total % CAP */
                                          /* once total >= CAP, oldest evicted */

static void journal_append(int drive_id, uint64_t lba, uint32_t count,
                           int op, int prior_vv, int new_vv)
{
    int slot = (int)(s_journal_total % BLOCK_JOURNAL_CAP);
    uint64_t tsc = timer_read_tsc();
    s_journal[slot].tsc                 = tsc;
    s_journal[slot].lba                 = lba;
    s_journal[slot].count               = count;
    s_journal[slot].drive_id            = drive_id;
    s_journal[slot].op                  = op;
    s_journal[slot].prior_vault_version = prior_vv;
    s_journal[slot].new_vault_version   = new_vv;
    s_journal_total++;

    /* Mirror to VAULT so the record survives a reboot. The
     * persistence layer no-ops until vault is mounted + initialized;
     * before that point we still keep the in-RAM record. */
    persistence_journal_append(tsc, drive_id, lba, count, op,
                               prior_vv, new_vv);
}

/*
 * Boot-time replay sink. Called by persistence_init() once per
 * persisted record so the in-RAM ring shows pre-reboot writes when
 * `masq-journal` runs. Does NOT touch VAULT (we're already loading
 * FROM vault) and does NOT bump any chain's vault_version (the live
 * registry is rebuilt elsewhere; vault_version comes from the chain
 * snapshot, not the journal).
 */
void block_chain_journal_replay_record(uint64_t tsc, int drive_id,
                                       uint64_t lba, uint32_t count,
                                       int op, int prior_vv, int new_vv)
{
    int slot = (int)(s_journal_total % BLOCK_JOURNAL_CAP);
    s_journal[slot].tsc                 = tsc;
    s_journal[slot].lba                 = lba;
    s_journal[slot].count               = count;
    s_journal[slot].drive_id            = drive_id;
    s_journal[slot].op                  = op;
    s_journal[slot].prior_vault_version = prior_vv;
    s_journal[slot].new_vault_version   = new_vv;
    s_journal_total++;
}

uint64_t block_chain_journal_total(void) { return s_journal_total; }

static const char *op_label(int op)
{
    switch (op) {
        case BLOCK_OP_READ:  return "READ";
        case BLOCK_OP_WRITE: return "WRITE";
        case BLOCK_OP_FLUSH: return "FLUSH";
        default:             return "?";
    }
}

void block_chain_dump_journal(int n)
{
    if (n <= 0) n = 16;
    if (n > BLOCK_JOURNAL_CAP) n = BLOCK_JOURNAL_CAP;

    uint64_t avail = s_journal_total < (uint64_t)BLOCK_JOURNAL_CAP
                     ? s_journal_total : (uint64_t)BLOCK_JOURNAL_CAP;
    if ((uint64_t)n > avail) n = (int)avail;

    if (n == 0) {
        kputs("  (journal empty)\n");
        return;
    }

    /* Walk newest-first: last entry index is (total-1) % CAP. */
    kputs("  masq journal (newest first, ");
    kput_dec((uint64_t)n);
    kputs(" of ");
    kput_dec(s_journal_total);
    kputs(" total):\n");

    for (int i = 0; i < n; i++) {
        uint64_t pos  = s_journal_total - 1 - (uint64_t)i;
        int      slot = (int)(pos % BLOCK_JOURNAL_CAP);
        journal_rec_t *r = &s_journal[slot];

        kputs("    tsc=");
        kput_dec(r->tsc);
        kputs(" drv=");
        kput_dec((uint64_t)r->drive_id);
        kputs(" op=");
        kputs(op_label(r->op));
        kputs(" lba=");
        kput_dec(r->lba);
        kputs(" cnt=");
        kput_dec((uint64_t)r->count);
        kputs(" vv=");
        kput_dec((uint64_t)r->prior_vault_version);
        kputs("->");
        kput_dec((uint64_t)r->new_vault_version);
        kputc('\n');
    }
}

/* ── Node resolves ─────────────────────────────────────────────── */

static void block_request_resolve(chain_node_t *self, void *input, void *output)
{
    (void)self;
    (void)input;
    block_stage_t *out = (block_stage_t *)output;
    out->req = &s_req;

    /* If no request is pending the rest of the pipeline will short-circuit
     * via req->error. This makes the chain safe to resolve speculatively
     * (chain_registry_tick scans all chains) without a stuck request. */
    if (!s_req.valid) {
        s_req.error = 1;
        return;
    }
    /* Defaults; addressing/journal/dma fill the rest. */
    s_req.kind          = BLOCK_KIND_NONE;
    s_req.sub_idx       = -1;
    s_req.sector_size   = 0;
    s_req.sectors_total = 0;
    s_req.error         = 0;
    s_req.journaled     = 0;
    s_req.prior_vault_version = 0;
    s_req.hw_rc         = -1;
}

static void addressing_resolve(chain_node_t *self, void *input, void *output)
{
    (void)self;
    block_stage_t *in  = (block_stage_t *)input;
    block_stage_t *out = (block_stage_t *)output;
    out->req = in->req;

    block_chain_request_t *r = in->req;
    if (!r || r->error || !r->valid) { if (r) r->error = 1; return; }

    block_drive_info_t info;
    if (block_drive_info(r->drive_id, &info) != 0) {
        r->error = 1;
        return;
    }
    r->kind          = info.kind;
    r->sub_idx       = info.sub_idx;
    r->sector_size   = info.sector_size;
    r->sectors_total = info.sectors;

    /* LBA range check (FLUSH ignores range -- it's whole-drive). */
    if (r->op != BLOCK_OP_FLUSH) {
        if (r->count == 0) { r->error = 1; return; }
        uint64_t end = r->lba + (uint64_t)r->count;
        if (end < r->lba || end > info.sectors) {  /* overflow OR OOB */
            r->error = 1;
            return;
        }
    }
}

static void masq_journal_resolve(chain_node_t *self, void *input, void *output)
{
    (void)self;
    block_stage_t *in  = (block_stage_t *)input;
    block_stage_t *out = (block_stage_t *)output;
    out->req = in->req;

    block_chain_request_t *r = in->req;
    if (!r || r->error || !r->valid) { if (r) r->error = 1; return; }

    /* Reads pass through with no provenance change. */
    if (r->op == BLOCK_OP_READ) {
        r->journaled = 0;
        return;
    }

    /* WRITE / FLUSH: capture prior vault_version, append a record,
     * bump vault_version on the chain. The journal records the pair
     * (prior, new) so a temporal walker can replay state changes. */
    chain_t *c = chain_get(CHAIN_BLOCK);
    int prior = c ? c->vault_version : 0;
    int next  = prior + 1;

    journal_append(r->drive_id, r->lba, r->count, r->op, prior, next);

    if (c) c->vault_version = next;

    r->prior_vault_version = prior;
    r->journaled           = 1;
}

static void hardware_dma_resolve(chain_node_t *self, void *input, void *output)
{
    (void)self;
    block_stage_t *in  = (block_stage_t *)input;
    block_stage_t *out = (block_stage_t *)output;
    out->req = in->req;

    block_chain_request_t *r = in->req;
    if (!r || r->error || !r->valid) { if (r) { r->error = 1; r->hw_rc = -1; } return; }

    int rc = -1;
    switch (r->op) {
    case BLOCK_OP_READ:
        switch (r->kind) {
            case BLOCK_KIND_NVME:    rc = nvme_read_drive(r->sub_idx, r->lba, r->count, r->buf); break;
            case BLOCK_KIND_AHCI:    rc = ahci_read_drive(r->sub_idx, r->lba, r->count, r->buf); break;
            case BLOCK_KIND_USB_MSC: rc = usb_msc_read(r->lba, r->count, r->buf);                break;
        }
        break;
    case BLOCK_OP_WRITE:
        switch (r->kind) {
            case BLOCK_KIND_NVME:    rc = nvme_write_drive(r->sub_idx, r->lba, r->count, (const void *)r->buf); break;
            case BLOCK_KIND_AHCI:    rc = ahci_write_drive(r->sub_idx, r->lba, r->count, (const void *)r->buf); break;
            case BLOCK_KIND_USB_MSC: rc = usb_msc_write(r->lba, r->count, (const void *)r->buf);                break;
        }
        break;
    case BLOCK_OP_FLUSH:
        switch (r->kind) {
            case BLOCK_KIND_NVME:    rc = nvme_flush_drive(r->sub_idx); break;
            case BLOCK_KIND_AHCI:    rc = ahci_flush_drive(r->sub_idx); break;
            case BLOCK_KIND_USB_MSC: rc = usb_msc_flush();              break;
        }
        break;
    }
    r->hw_rc = rc;
    if (rc != 0) r->error = 1;
}

/* ── Registration ──────────────────────────────────────────────── */

int block_chain_register(int parent_id)
{
    CHAIN_BLOCK = chain_create("block", parent_id, MASQ_INTERNAL);
    if (CHAIN_BLOCK < 0) {
        kputs("[block_chain] failed to create block chain\n");
        return -1;
    }

    chain_add_node(CHAIN_BLOCK, "block_request",
                   "block_request", "block_stage",
                   block_request_resolve);
    chain_add_node(CHAIN_BLOCK, "addressing",
                   "block_stage", "block_stage",
                   addressing_resolve);
    chain_add_node(CHAIN_BLOCK, "masq_journal",
                   "block_stage", "block_stage",
                   masq_journal_resolve);
    chain_add_node(CHAIN_BLOCK, "hardware_dma",
                   "block_stage", "tx_completion",
                   hardware_dma_resolve);
    return 0;
}

/* ── Submit (compat-shim entrypoint) ───────────────────────────── */

int block_chain_submit(int drive_id, uint64_t lba, uint32_t count,
                       int op, void *buf)
{
    /* Pre-init / unregistered chain: caller should fall back to the
     * direct path. block.c handles that decision. */
    if (CHAIN_BLOCK < 0) return -1;

    s_req.valid    = 1;
    s_req.drive_id = drive_id;
    s_req.lba      = lba;
    s_req.count    = count;
    s_req.op       = op;
    s_req.buf      = buf;
    s_req.error    = 0;
    s_req.hw_rc    = -1;
    s_req.journaled = 0;

    int rc = chain_resolve(CHAIN_BLOCK);

    /* Consume the slot regardless of outcome. */
    int err     = s_req.error;
    int hw_rc   = s_req.hw_rc;
    s_req.valid = 0;

    if (rc != 0) return -1;
    if (err)     return (hw_rc != 0) ? hw_rc : -1;
    return hw_rc;
}
