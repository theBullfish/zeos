/*
 * Zeos -- CFA-native disk encryption. Public API.
 *
 * Keys are SOVEREIGN handles; encrypted regions are CFA-addressed; the
 * crypto_transform node in CHAIN_BLOCK is the only place plaintext meets
 * ciphertext. PIN derives the key via PBKDF2-HMAC-SHA256 at boot.
 * AES-256-XTS with lba tweak.
 *
 * NOT LUKS-shaped. The Zeos primitive is the CFA handle plus the
 * chain_resolve gate, not a partition header.
 */
#ifndef ZEOS_CRYPTO_DISK_H
#define ZEOS_CRYPTO_DISK_H

#include <stdint.h>

/* Cipher selectors. AES-XTS-256 uses 64-byte raw key (split 32/32). */
#define CRYPTO_CIPHER_NONE      0
#define CRYPTO_CIPHER_AES_XTS_256 1

#define CRYPTO_REGION_NAME_MAX  32
#define CRYPTO_MAX_REGIONS      8

/* Region descriptor on disk + in-RAM. lba_count == 0 means unused slot.
 * cipher selects the transform; cipher == CRYPTO_CIPHER_NONE acts as a
 * tombstone. Per-region salt allows future per-region key derivation;
 * v1 derives a single master key from the cold-boot PIN and uses it
 * for every region. The region key handle is bound at boot. */
typedef struct {
    char     name[CRYPTO_REGION_NAME_MAX];
    int      drive_id;          /* -1 = match any drive (default 0) */
    uint64_t lba_start;
    uint64_t lba_count;
    uint16_t cipher;
    uint16_t _pad0;
    uint32_t access_count;      /* MasQ-recorded access counter */
    uint32_t denied_count;      /* perceive-check failures */
    uint8_t  reserved[24];
} crypto_region_t;

/* Init -- after vault_mount, after CHAIN_BLOCK exists, after the
 * cold-boot PIN is validated. Loads /crypto/salt (or generates+persists
 * one on first boot), derives the AES-XTS-256 master key from the PIN,
 * wraps it in a SOVEREIGN CFA handle, loads /crypto/regions, and
 * arms the crypto_transform node. */
void crypto_disk_init(const char *pin, int pin_len);

/* True once init has run successfully. */
int  crypto_disk_armed(void);

/* Region lifecycle. */
int  crypto_disk_create_region(const char *name, int drive_id,
                               uint64_t lba_start, uint64_t lba_count);
int  crypto_disk_open_region(const char *name);          /* returns id, -1 miss */
int  crypto_disk_destroy_region(const char *name);

/* Lookup: returns region id (>= 0) if (drive_id, lba) hits an encrypted
 * region, else -1. Used by the CHAIN_BLOCK crypto_transform node. */
int  crypto_disk_lookup(int drive_id, uint64_t lba, uint32_t count);

/* Per-block transform. Called by the crypto_transform node in
 * block_chain.c. mode = 0 -> decrypt (READ), mode = 1 -> encrypt (WRITE).
 * `block_count` is in 512-byte sectors. Operates in place when src==dst.
 * Returns 0 on success, -1 on resolve failure (key handle denied),
 * -2 on cipher error. The denied case is logged into the region's
 * denied_count and the journal. */
int  crypto_disk_transform(int region_id, int mode, uint64_t lba,
                           uint32_t block_count, uint32_t sector_size,
                           const void *src, void *dst);

/* Re-derive master key from a new PIN and re-encrypt every region in
 * place (read+decrypt-with-old-key, encrypt-with-new-key, write).
 * Long operation; intended to run with the lockscreen modal up.
 * Returns 0 on success. */
int  crypto_disk_rekey(const char *new_pin, int new_pin_len);

/* Selftest hook. Prints the canonical line. */
void crypto_disk_print_selftest_line(void);

/* Settings registry hook. */
void crypto_disk_settings_register(void);

/* Shell command. `crypto status | encrypt-region <name> <start> <count> | rekey`. */
void crypto_cmd(const char *args);

/* VAULT helpers -- transparent encrypt/decrypt when a /vault region is
 * registered. Otherwise pass through to vault_write / vault_read. */
int  vault_put_encrypted(const char *path, const void *data, uint32_t size);
int  vault_get_encrypted(const char *path, void *buf, uint32_t size);

/* Stats for the inspector + selftest. */
int  crypto_disk_region_count(void);
int  crypto_disk_region_at(int idx, crypto_region_t *out);
uint32_t crypto_disk_total_accesses(void);

#endif /* ZEOS_CRYPTO_DISK_H */
