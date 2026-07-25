/*
 * Zeos -- Chain-native block I/O (header)
 *
 * Pipeline: block_request -> addressing -> masq_journal -> hardware_dma
 * NVMe / AHCI / USB-MSC drivers are the hardware_dma backend.
 */
#ifndef ZEOS_BLOCK_CHAIN_H
#define ZEOS_BLOCK_CHAIN_H

#include <stdint.h>

/* Op codes for block requests. */
#define BLOCK_OP_READ  0
#define BLOCK_OP_WRITE 1
#define BLOCK_OP_FLUSH 2

/* Register the block chain under the given parent (CHAIN_CPU).
 * Returns 0 on success. */
int block_chain_register(int parent_id);

/* Submit a request through the chain. The compat shims in block.c call
 * this. Returns 0 on success, -1 on failure. */
int block_chain_submit(int drive_id, uint64_t lba, uint32_t count,
                       int op, void *buf);

/* Print the last n masq journal entries to kprint. */
void block_chain_dump_journal(int n);

/* Number of journal records ever written (including evicted). */
uint64_t block_chain_journal_total(void);

/* Suppress masq journaling for block writes issued by the persistence flush
 * itself (A.9 fix). Journaling the act of persisting the vault/journal to disk
 * is circular: the write path's journal node (masq_journal_resolve) calls
 * persistence_journal_append -> vault_append, which re-takes g_vault_lock /
 * g_persist_lock already held across vault_sync -> vault_persist_flush, a hard
 * self-deadlock (spins forever, no NVMe timeout -- this was the A.9 tick-4
 * hang). vault_persist_flush wraps its write loop in suppress(1)/suppress(0).
 * Nesting-safe (depth counter). */
void block_chain_set_journal_suppressed(int on);

#endif /* ZEOS_BLOCK_CHAIN_H */
