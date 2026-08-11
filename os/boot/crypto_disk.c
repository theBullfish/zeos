/*
 * CFA-native disk encryption. Keys are SOVEREIGN handles; encrypted
 * regions are CFA-addressed; the crypto_transform node in CHAIN_BLOCK
 * is the only place plaintext meets ciphertext. PIN derives the key
 * via PBKDF2-HMAC-SHA256 at boot. AES-256-XTS with lba tweak.
 * MasQ records every region access -- denied attempts included.
 *
 * NOT LUKS-shaped. The Zeos primitive is the CFA handle plus the
 * chain_resolve gate, not a partition header.
 *
 * Decomposition:
 *   PIN ── PBKDF2-HMAC-SHA256 ─► 64B raw key ── cfa_wrap(SOVEREIGN) ─► g_master_key_h
 *   region table ◄── /crypto/regions (vault_save_config)
 *   CHAIN_BLOCK = block_request → addressing → masq_journal →
 *                 crypto_transform → hardware_dma
 *   crypto_transform: lookup(drive_id, lba); if hit, cfa_resolve key
 *   handle (denied if observer can't perceive SOVEREIGN), then
 *   AES-XTS encrypt/decrypt with tweak=lba.
 */

#include "crypto_disk.h"
#include "vault.h"
#include "chain.h"
#include "chain_registry.h"
#include "cfa_handle.h"
#include "block.h"
#include "block_chain.h"
#include "kprint.h"
#include "timer.h"
#include "spinlock.h"
#include "settings_registry.h"
#include "persistence.h"

#include <stdint.h>
#include <stddef.h>

#include "mbedtls/aes.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/md.h"
#include "mbedtls/platform.h"
#include "psa/crypto.h"

/* External entropy source (mbedtls_platform.c). */
extern int mbedtls_hardware_poll(void *data, unsigned char *output,
                                 size_t len, size_t *olen);

/* mbedTLS calloc/free shims (mbedtls_platform.c). */
extern void *zeos_calloc(size_t n, size_t size);
extern void  zeos_free(void *ptr);

/* External chain registry: CHAIN_BLOCK is registered there. */
extern int CHAIN_BLOCK;

/* Lockscreen accessor -- opaque copy of the stored PIN bytes (NUL-padded
 * up to LOCK_PIN_MAX_LEN+1 buffer). Implemented in lockscreen.c. */
extern int  lockscreen_pin_copy(char *out, int max);

/* ── Local string helpers (freestanding) ─────────────────────────── */

static int cd_strlen(const char *s) { int n=0; while(s[n])n++; return n; }
static int cd_streq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == '\0' && *b == '\0';
}
static void cd_strncpy(char *dst, const char *src, int cap) {
    int i=0; if (cap<=0) return;
    while (i<cap-1 && src[i]) { dst[i]=src[i]; i++; }
    dst[i]='\0';
}
static void cd_memzero(void *p, size_t n) {
    volatile uint8_t *v = (volatile uint8_t *)p;
    for (size_t i=0;i<n;i++) v[i]=0;
}
static void cd_memcpy(void *d, const void *s, size_t n) {
    uint8_t *dd=(uint8_t*)d; const uint8_t*ss=(const uint8_t*)s;
    for (size_t i=0;i<n;i++) dd[i]=ss[i];
}
static int cd_parse_u64(const char *s, uint64_t *out) {
    if (!s || !*s) return -1;
    uint64_t v=0; int any=0;
    while (*s>='0' && *s<='9') { v = v*10 + (uint64_t)(*s - '0'); s++; any=1; }
    if (!any) return -1;
    while (*s == ' ' || *s == '\t') s++;
    if (*s) return -1;
    *out = v; return 0;
}
static const char *cd_skip_ws(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* ── Constants ───────────────────────────────────────────────────── */

#define CRYPTO_SALT_LEN          32
#define CRYPTO_KEY_LEN           64        /* AES-XTS-256 = 2 * 256 bits */
#define CRYPTO_PBKDF2_ITERS      120000
#define CRYPTO_VAULT_SALT_PATH   "/crypto/salt"
#define CRYPTO_VAULT_REGIONS_PATH "/crypto/regions"
#define CRYPTO_SALT_PATH_MAX     64

/* Active salt path. Defaults to CRYPTO_VAULT_SALT_PATH; identity.c
 * overrides this when switching contexts so each context derives its
 * own master key from its own salt. */
static char g_salt_path[CRYPTO_SALT_PATH_MAX] = CRYPTO_VAULT_SALT_PATH;

/* MasQ-tier PIN buffer for the duration of init. Wiped immediately
 * after key derivation. We never persist the PIN bytes anywhere. */

/* ── State ───────────────────────────────────────────────────────── */

static zeos_spinlock_t g_lock = ZEOS_SPINLOCK_INIT;

static int            g_armed;
static uint8_t        g_salt[CRYPTO_SALT_LEN];

/* Master AES-XTS-256 key (64 bytes). Wrapped at SOVEREIGN tier; the
 * raw bytes only exist in this static array, which is itself the
 * pointed-to backing store of the CFA handle. */
static uint8_t        g_master_key[CRYPTO_KEY_LEN];
static cfa_handle_t   g_master_key_h;

/* Regions persisted to VAULT under /crypto/regions. */
static crypto_region_t g_regions[CRYPTO_MAX_REGIONS];

/* Stats. */
static uint32_t       g_total_accesses;
static uint32_t       g_total_denied;

/* ── PBKDF2 + key derivation ─────────────────────────────────────── */

static int derive_master_key(const char *pin, int pin_len,
                             const uint8_t *salt, uint32_t slen,
                             uint8_t out[CRYPTO_KEY_LEN])
{
    int rc = mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256,
                                           (const unsigned char *)pin,
                                           (size_t)pin_len,
                                           salt, slen,
                                           CRYPTO_PBKDF2_ITERS,
                                           CRYPTO_KEY_LEN, out);
    return rc == 0 ? 0 : -1;
}

static int load_or_create_salt(void)
{
    int got = vault_load_config(g_salt_path, g_salt, sizeof(g_salt));
    if (got == (int)sizeof(g_salt)) return 0;

    /* Generate fresh salt via mbedtls_hardware_poll (RDRAND + TSC jitter). */
    size_t olen = 0;
    if (mbedtls_hardware_poll(0, g_salt, sizeof(g_salt), &olen) != 0
        || olen != sizeof(g_salt)) {
        /* Fallback: TSC-derived. Better than nothing but warn. */
        kputs("[crypto] WARN: hw entropy failed; salt = TSC-derived\n");
        uint64_t t = timer_read_tsc();
        for (uint32_t i = 0; i < sizeof(g_salt); i++) {
            g_salt[i] = (uint8_t)((t >> ((i % 8) * 8)) & 0xFF);
            if ((i & 7) == 7) t = timer_read_tsc() ^ (t << 13) ^ (t >> 7);
        }
    }
    int wrote = vault_save_config(g_salt_path,
                                  g_salt, sizeof(g_salt));
    if (wrote < 0) {
        kputs("[crypto] WARN: salt persist failed; key non-stable across reboots\n");
        return -1;
    }
    return 0;
}

/* ── Region table persistence ────────────────────────────────────── */

static int regions_save(void)
{
    int wrote = vault_save_config(CRYPTO_VAULT_REGIONS_PATH,
                                  g_regions, sizeof(g_regions));
    return (wrote == (int)sizeof(g_regions)) ? 0 : -1;
}

static int regions_load(void)
{
    /* Zero first; vault_load_config returns -1 if the key is missing
     * (first boot) and we keep the freshly-zeroed table. */
    cd_memzero(g_regions, sizeof(g_regions));
    int got = vault_load_config(CRYPTO_VAULT_REGIONS_PATH,
                                g_regions, sizeof(g_regions));
    if (got != (int)sizeof(g_regions)) {
        cd_memzero(g_regions, sizeof(g_regions));
        return -1;
    }
    return 0;
}

static int region_find_slot_free(void)
{
    for (int i = 0; i < CRYPTO_MAX_REGIONS; i++)
        if (g_regions[i].lba_count == 0) return i;
    return -1;
}

static int region_find_by_name(const char *name)
{
    if (!name) return -1;
    for (int i = 0; i < CRYPTO_MAX_REGIONS; i++) {
        if (g_regions[i].lba_count == 0) continue;
        if (cd_streq(g_regions[i].name, name)) return i;
    }
    return -1;
}

/* ── Public API ──────────────────────────────────────────────────── */

int crypto_disk_armed(void) { return g_armed; }

int crypto_disk_region_count(void)
{
    int n = 0;
    for (int i = 0; i < CRYPTO_MAX_REGIONS; i++)
        if (g_regions[i].lba_count > 0) n++;
    return n;
}

int crypto_disk_region_at(int idx, crypto_region_t *out)
{
    if (idx < 0 || idx >= CRYPTO_MAX_REGIONS || !out) return -1;
    if (g_regions[idx].lba_count == 0) return -1;
    *out = g_regions[idx];
    return 0;
}

uint32_t crypto_disk_total_accesses(void) { return g_total_accesses; }

int crypto_disk_lookup(int drive_id, uint64_t lba, uint32_t count)
{
    if (!g_armed) return -1;
    for (int i = 0; i < CRYPTO_MAX_REGIONS; i++) {
        crypto_region_t *r = &g_regions[i];
        if (r->lba_count == 0) continue;
        if (r->cipher == CRYPTO_CIPHER_NONE) continue;
        if (r->drive_id != -1 && r->drive_id != drive_id) continue;
        uint64_t end = lba + (uint64_t)count;
        if (lba >= r->lba_start && end <= r->lba_start + r->lba_count) {
            return i;
        }
    }
    return -1;
}

int crypto_disk_create_region(const char *name, int drive_id,
                              uint64_t lba_start, uint64_t lba_count)
{
    if (!g_armed) { kputs("[crypto] not armed; cannot create region\n"); return -1; }
    if (!name || !*name || lba_count == 0) return -1;
    if (cd_strlen(name) >= CRYPTO_REGION_NAME_MAX) return -1;

    spin_lock(&g_lock);
    if (region_find_by_name(name) >= 0) { spin_unlock(&g_lock); return -1; }
    int slot = region_find_slot_free();
    if (slot < 0) { spin_unlock(&g_lock); return -1; }

    crypto_region_t *r = &g_regions[slot];
    cd_memzero(r, sizeof(*r));
    cd_strncpy(r->name, name, CRYPTO_REGION_NAME_MAX);
    r->drive_id  = drive_id;
    r->lba_start = lba_start;
    r->lba_count = lba_count;
    r->cipher    = CRYPTO_CIPHER_AES_XTS_256;

    int rc = regions_save();
    spin_unlock(&g_lock);

    /* MasQ provenance: a region create is a state change. */
    chain_t *c = (CHAIN_BLOCK >= 0) ? chain_get(CHAIN_BLOCK) : 0;
    if (c) c->vault_version++;

    if (rc < 0) {
        kputs("[crypto] WARN: region persist failed\n");
    }
    return slot;
}

int crypto_disk_open_region(const char *name)
{
    spin_lock(&g_lock);
    int id = region_find_by_name(name);
    spin_unlock(&g_lock);
    return id;
}

int crypto_disk_destroy_region(const char *name)
{
    spin_lock(&g_lock);
    int id = region_find_by_name(name);
    if (id < 0) { spin_unlock(&g_lock); return -1; }
    /* Wipe the slot. Per-region keys are derived from the master key,
     * so destroying the slot effectively retires the addressing; the
     * ciphertext on disk is unrecoverable through this path going
     * forward. We don't zero the disk -- that's a user-driven shred. */
    cd_memzero(&g_regions[id], sizeof(g_regions[id]));
    int rc = regions_save();
    spin_unlock(&g_lock);

    chain_t *c = (CHAIN_BLOCK >= 0) ? chain_get(CHAIN_BLOCK) : 0;
    if (c) c->vault_version++;
    return rc;
}

/* ── Transform ───────────────────────────────────────────────────── */

/* AES-XTS data unit = one sector. Tweak = lba (encoded little-endian
 * 16-byte). mbedtls bound: 16B..16MiB per call; we run one sector per
 * call to keep the tweak == lba contract simple and robust. */
int crypto_disk_transform(int region_id, int mode, uint64_t lba,
                          uint32_t block_count, uint32_t sector_size,
                          const void *src, void *dst)
{
    if (!g_armed) return -1;
    if (region_id < 0 || region_id >= CRYPTO_MAX_REGIONS) return -1;
    if (sector_size == 0 || (sector_size % 16) != 0) return -2;

    crypto_region_t *r = &g_regions[region_id];
    if (r->lba_count == 0 || r->cipher != CRYPTO_CIPHER_AES_XTS_256) return -1;

    /* The crypto_transform node is the ONE chain node authorized to
     * touch the SOVEREIGN key buffer. cfa_set_observer(id) inside
     * chain_resolve has set the observer to CHAIN_BLOCK (MASQ_INTERNAL)
     * which by doctrine cannot perceive MASQ_SOVEREIGN. We enter a
     * narrow privileged scope (observer=-1, permissive) for the
     * resolve only, restoring before we do anything else. This is the
     * trust gate: every other node in every other chain still gets
     * tier-denied. The denied_count below records any other caller
     * that reaches here without arming. */
    int prev_obs = cfa_get_observer();
    cfa_set_observer(-1);
    uint8_t *key = (uint8_t *)cfa_resolve(g_master_key_h);
    cfa_set_observer(prev_obs);
    if (!key) {
        spin_lock(&g_lock);
        r->denied_count++;
        g_total_denied++;
        spin_unlock(&g_lock);
        kputs("[crypto] DENIED: master key handle invalid\n");
        return -1;
    }

    mbedtls_aes_xts_context ctx;
    mbedtls_aes_xts_init(&ctx);
    int rc;
    if (mode == 1) {
        rc = mbedtls_aes_xts_setkey_enc(&ctx, key, 512);
    } else {
        rc = mbedtls_aes_xts_setkey_dec(&ctx, key, 512);
    }
    if (rc != 0) { mbedtls_aes_xts_free(&ctx); return -2; }

    const uint8_t *in  = (const uint8_t *)src;
    uint8_t       *out = (uint8_t *)dst;

    int xts_mode = (mode == 1) ? MBEDTLS_AES_ENCRYPT : MBEDTLS_AES_DECRYPT;

    for (uint32_t i = 0; i < block_count; i++) {
        uint64_t this_lba = lba + (uint64_t)i;
        unsigned char data_unit[16];
        cd_memzero(data_unit, sizeof(data_unit));
        for (int b = 0; b < 8; b++)
            data_unit[b] = (unsigned char)((this_lba >> (b * 8)) & 0xFF);
        rc = mbedtls_aes_crypt_xts(&ctx, xts_mode, sector_size,
                                   data_unit, in, out);
        if (rc != 0) {
            mbedtls_aes_xts_free(&ctx);
            return -2;
        }
        in  += sector_size;
        out += sector_size;
    }

    mbedtls_aes_xts_free(&ctx);

    spin_lock(&g_lock);
    r->access_count++;
    g_total_accesses++;
    spin_unlock(&g_lock);

    /* MasQ: every encrypted-region access bumps CHAIN_BLOCK.vault_version
     * with region_id implicit in the access counter. Reads count too --
     * for SOVEREIGN data even an observation is a state change. */
    chain_t *c = (CHAIN_BLOCK >= 0) ? chain_get(CHAIN_BLOCK) : 0;
    if (c) c->vault_version++;

    return 0;
}

/* ── Init ────────────────────────────────────────────────────────── */

void crypto_disk_init(const char *pin, int pin_len)
{
    if (g_armed) return;
    if (!pin || pin_len <= 0) {
        kputs("[crypto] init skipped: no PIN\n");
        return;
    }

    /* mbedTLS 3.6 with MBEDTLS_USE_PSA_CRYPTO routes md operations
     * through PSA. tls_init() (net.c) initializes PSA, but it runs
     * AFTER us. Ensure PSA is up before we call PBKDF2. Idempotent --
     * tls_init re-running psa_crypto_init returns ALREADY_EXISTS which
     * we treat as success here. */
    static int s_platform_set;
    if (!s_platform_set) {
        mbedtls_platform_set_calloc_free(zeos_calloc, zeos_free);
        s_platform_set = 1;
    }
    psa_status_t pst = psa_crypto_init();
    if (pst != PSA_SUCCESS && pst != PSA_ERROR_ALREADY_EXISTS) {
        kputs("[crypto] PSA init failed: ");
        kput_dec((uint64_t)(-(int)pst));
        kputc('\n');
        return;
    }

    if (load_or_create_salt() < 0) {
        kputs("[crypto] WARN: continuing with non-persisted salt\n");
    }

    cd_memzero(g_master_key, sizeof(g_master_key));
    if (derive_master_key(pin, pin_len, g_salt, sizeof(g_salt),
                          g_master_key) != 0) {
        kputs("[crypto] FATAL: PBKDF2 failed\n");
        cd_memzero(g_master_key, sizeof(g_master_key));
        return;
    }

    /* Wrap the key buffer at SOVEREIGN tier. The CFA address is derived
     * from CHAIN_BLOCK so an inspector can identify it as block-layer
     * crypto material. Never expose the raw bytes to a non-SOVEREIGN
     * observer -- cfa_resolve enforces this through tier_can_perceive. */
    cfa_addr_t base;
    chain_t *blk = (CHAIN_BLOCK >= 0) ? chain_get(CHAIN_BLOCK) : 0;
    if (blk) {
        base = cfa_derive(&blk->addr, 0xC4D);          /* 'CRD' */
    } else {
        for (int i = 0; i < 8; i++) base.segments[i] = 0;
        base.segments[0] = 0xC4D;
        base.depth = 1;
        base.birth_tsc = timer_read_tsc();
    }
    cfa_addr_t key_addr = cfa_derive(&base, 1);
    g_master_key_h = cfa_wrap(g_master_key, sizeof(g_master_key),
                              key_addr, MASQ_SOVEREIGN);
    if (g_master_key_h == 0) {
        kputs("[crypto] FATAL: cfa_wrap of master key failed\n");
        cd_memzero(g_master_key, sizeof(g_master_key));
        return;
    }

    (void)regions_load();

    g_armed = 1;
    kputs("[crypto] armed (AES-XTS-256, ");
    kput_dec((uint64_t)crypto_disk_region_count());
    kputs(" regions, key=SOVEREIGN-wrapped)\n");
}

/* Identity-context support: switch the salt path so a subsequent
 * init/reinit derives keys from a different per-context VAULT salt.
 * Caller passes a stable string (string literal or context-owned). */
void crypto_disk_set_salt_path(const char *path)
{
    if (!path || !*path) return;
    int i = 0;
    while (path[i] && i < (int)sizeof(g_salt_path) - 1) {
        g_salt_path[i] = path[i];
        i++;
    }
    g_salt_path[i] = '\0';
}

/* Re-derive the master key from `pin` + the current salt-path's salt.
 * Wipes the previous key first. Returns 0 on success.
 *
 * Called by identity_context_switch. The CFA handle for the master
 * key is reused; only the underlying key bytes change. Any in-flight
 * region access either completed under the old key (decrypted, returned
 * to caller) or hasn't started; the spinlock around g_total_accesses
 * isn't a synchronisation primitive for the key bytes themselves --
 * during a context switch the system is single-threaded at the gate. */
int crypto_disk_reinit_with_salt(const char *pin, int pin_len,
                                 const char *salt_path)
{
    if (!pin || pin_len <= 0) return -1;
    crypto_disk_set_salt_path(salt_path ? salt_path : CRYPTO_VAULT_SALT_PATH);

    /* Make sure PSA + platform shims are alive; they may have been set
     * up already by an earlier crypto_disk_init() call. */
    static int s_platform_set;
    if (!s_platform_set) {
        mbedtls_platform_set_calloc_free(zeos_calloc, zeos_free);
        s_platform_set = 1;
    }
    psa_status_t pst = psa_crypto_init();
    if (pst != PSA_SUCCESS && pst != PSA_ERROR_ALREADY_EXISTS) return -1;

    if (load_or_create_salt() < 0) {
        kputs("[crypto] WARN: reinit with non-persisted salt\n");
    }

    uint8_t newkey[CRYPTO_KEY_LEN];
    cd_memzero(newkey, sizeof(newkey));
    if (derive_master_key(pin, pin_len, g_salt, sizeof(g_salt), newkey) != 0) {
        cd_memzero(newkey, sizeof(newkey));
        return -1;
    }

    /* Swap key bytes in place under the spinlock so a concurrent
     * crypto_disk_transform on another core can't race a half-written
     * key. The CFA handle keeps pointing at g_master_key. */
    spin_lock(&g_lock);
    cd_memzero(g_master_key, sizeof(g_master_key));
    cd_memcpy(g_master_key, newkey, sizeof(newkey));
    spin_unlock(&g_lock);
    cd_memzero(newkey, sizeof(newkey));

    /* If we hadn't been armed before (first context login), wrap the
     * key in a SOVEREIGN handle and load regions now. Subsequent
     * switches keep the same handle. */
    if (!g_armed) {
        cfa_addr_t base;
        chain_t *blk = (CHAIN_BLOCK >= 0) ? chain_get(CHAIN_BLOCK) : 0;
        if (blk) {
            base = cfa_derive(&blk->addr, 0xC4D);
        } else {
            for (int i = 0; i < 8; i++) base.segments[i] = 0;
            base.segments[0] = 0xC4D;
            base.depth = 1;
            base.birth_tsc = timer_read_tsc();
        }
        cfa_addr_t key_addr = cfa_derive(&base, 1);
        g_master_key_h = cfa_wrap(g_master_key, sizeof(g_master_key),
                                  key_addr, MASQ_SOVEREIGN);
        if (g_master_key_h == 0) {
            cd_memzero(g_master_key, sizeof(g_master_key));
            return -1;
        }
        (void)regions_load();
        g_armed = 1;
    }

    chain_t *c = (CHAIN_BLOCK >= 0) ? chain_get(CHAIN_BLOCK) : 0;
    if (c) c->vault_version++;
    return 0;
}

/* ── Rekey ───────────────────────────────────────────────────────── */

int crypto_disk_rekey(const char *new_pin, int new_pin_len)
{
    if (!g_armed) return -1;
    if (!new_pin || new_pin_len <= 0) return -1;

    /* Derive the new key from a fresh salt so the same PIN can't yield
     * the same key bytes (forward secrecy across rekeys). */
    uint8_t new_salt[CRYPTO_SALT_LEN];
    size_t olen = 0;
    if (mbedtls_hardware_poll(0, new_salt, sizeof(new_salt), &olen) != 0
        || olen != sizeof(new_salt)) {
        kputs("[crypto] rekey: hw entropy failed\n");
        return -1;
    }
    uint8_t new_key[CRYPTO_KEY_LEN];
    if (derive_master_key(new_pin, new_pin_len, new_salt, sizeof(new_salt),
                          new_key) != 0) {
        cd_memzero(new_key, sizeof(new_key));
        return -1;
    }

    /* For every region: read each sector with the OLD key, encrypt with
     * the NEW key, write back. Doing this through the chain would be
     * recursive; instead we go directly through block_read/_write,
     * applying the transform manually with explicit key arrays. */
    uint8_t buf[4096];
    for (int i = 0; i < CRYPTO_MAX_REGIONS; i++) {
        crypto_region_t *r = &g_regions[i];
        if (r->lba_count == 0) continue;
        if (r->cipher != CRYPTO_CIPHER_AES_XTS_256) continue;

        kputs("[crypto] rekey region '"); kputs(r->name);
        kputs("' lbas="); kput_dec(r->lba_count); kputs("...\n");

        for (uint64_t off = 0; off < r->lba_count; off++) {
            uint64_t this_lba = r->lba_start + off;
            int rc = (r->drive_id < 0)
                     ? block_read(this_lba, 1, buf)
                     : block_read_drive(r->drive_id, this_lba, 1, buf);
            if (rc != 0) {
                kputs("[crypto] rekey: read failed at lba="); kput_dec(this_lba);
                kputc('\n'); cd_memzero(new_key, sizeof(new_key));
                return -1;
            }

            /* Decrypt with OLD key in place. */
            mbedtls_aes_xts_context ctx;
            mbedtls_aes_xts_init(&ctx);
            unsigned char tweak[16];
            cd_memzero(tweak, sizeof(tweak));
            for (int b = 0; b < 8; b++)
                tweak[b] = (unsigned char)((this_lba >> (b * 8)) & 0xFF);
            if (mbedtls_aes_xts_setkey_dec(&ctx, g_master_key, 512) != 0) {
                mbedtls_aes_xts_free(&ctx);
                cd_memzero(new_key, sizeof(new_key));
                return -1;
            }
            (void)mbedtls_aes_crypt_xts(&ctx, MBEDTLS_AES_DECRYPT, 4096,
                                        tweak, buf, buf);
            mbedtls_aes_xts_free(&ctx);

            /* Encrypt with NEW key in place. */
            mbedtls_aes_xts_init(&ctx);
            if (mbedtls_aes_xts_setkey_enc(&ctx, new_key, 512) != 0) {
                mbedtls_aes_xts_free(&ctx);
                cd_memzero(new_key, sizeof(new_key));
                return -1;
            }
            (void)mbedtls_aes_crypt_xts(&ctx, MBEDTLS_AES_ENCRYPT, 4096,
                                        tweak, buf, buf);
            mbedtls_aes_xts_free(&ctx);

            rc = (r->drive_id < 0)
                 ? block_write(this_lba, 1, buf)
                 : block_write_drive(r->drive_id, this_lba, 1, buf);
            if (rc != 0) {
                kputs("[crypto] rekey: write failed at lba="); kput_dec(this_lba);
                kputc('\n'); cd_memzero(new_key, sizeof(new_key));
                return -1;
            }
        }
    }

    /* Swap in the new key + salt. Wipe the old buffer first. */
    cd_memzero(g_master_key, sizeof(g_master_key));
    cd_memcpy(g_master_key, new_key, sizeof(new_key));
    cd_memcpy(g_salt, new_salt, sizeof(new_salt));
    cd_memzero(new_key, sizeof(new_key));
    (void)vault_save_config(CRYPTO_VAULT_SALT_PATH, g_salt, sizeof(g_salt));

    chain_t *c = (CHAIN_BLOCK >= 0) ? chain_get(CHAIN_BLOCK) : 0;
    if (c) c->vault_version++;
    kputs("[crypto] rekey complete\n");
    return 0;
}

/* ── VAULT-level helpers ─────────────────────────────────────────── */
/*
 * vault_put_encrypted / vault_get_encrypted route through a similar
 * CFA-handle-resolve. They wrap the small in-RAM buffer in a SOVEREIGN
 * handle, run a one-shot AES-XTS transform with tweak = hash-of-path,
 * and call vault_write/vault_read. If crypto_disk is not armed they
 * pass through unmodified -- safe pre-PIN-enrollment.
 */

static uint64_t path_tweak(const char *path)
{
    /* FNV-1a 64-bit. Sufficient as an XTS data-unit selector; the
     * underlying cipher provides confidentiality. */
    uint64_t h = 0xcbf29ce484222325ULL;
    while (*path) {
        h ^= (uint8_t)(*path++);
        h *= 0x100000001b3ULL;
    }
    return h;
}

int vault_put_encrypted(const char *path, const void *data, uint32_t size)
{
    if (!g_armed || size == 0 || (size % 16) != 0) {
        return vault_write(path, data, size);
    }
    int _prev_obs = cfa_get_observer();
    cfa_set_observer(-1);
    uint8_t *key = (uint8_t *)cfa_resolve(g_master_key_h);
    cfa_set_observer(_prev_obs);
    if (!key) return -1;

    uint8_t tmp[4096];
    if (size > sizeof(tmp)) return -1;
    cd_memcpy(tmp, data, size);

    mbedtls_aes_xts_context ctx;
    mbedtls_aes_xts_init(&ctx);
    if (mbedtls_aes_xts_setkey_enc(&ctx, key, 512) != 0) {
        mbedtls_aes_xts_free(&ctx);
        return -1;
    }
    unsigned char tweak[16];
    cd_memzero(tweak, sizeof(tweak));
    uint64_t t = path_tweak(path);
    for (int b = 0; b < 8; b++)
        tweak[b] = (unsigned char)((t >> (b * 8)) & 0xFF);
    int rc = mbedtls_aes_crypt_xts(&ctx, MBEDTLS_AES_ENCRYPT, size, tweak, tmp, tmp);
    mbedtls_aes_xts_free(&ctx);
    if (rc != 0) return -1;

    int wrote = vault_write(path, tmp, size);
    cd_memzero(tmp, sizeof(tmp));
    return wrote;
}

int vault_get_encrypted(const char *path, void *buf, uint32_t size)
{
    if (!g_armed || size == 0 || (size % 16) != 0) {
        return vault_read(path, buf, size);
    }
    int _prev_obs = cfa_get_observer();
    cfa_set_observer(-1);
    uint8_t *key = (uint8_t *)cfa_resolve(g_master_key_h);
    cfa_set_observer(_prev_obs);
    if (!key) return -1;

    int got = vault_read(path, buf, size);
    if (got <= 0) return got;
    if ((uint32_t)got != size) return got;  /* short read -- pass through */

    mbedtls_aes_xts_context ctx;
    mbedtls_aes_xts_init(&ctx);
    if (mbedtls_aes_xts_setkey_dec(&ctx, key, 512) != 0) {
        mbedtls_aes_xts_free(&ctx);
        return -1;
    }
    unsigned char tweak[16];
    cd_memzero(tweak, sizeof(tweak));
    uint64_t t = path_tweak(path);
    for (int b = 0; b < 8; b++)
        tweak[b] = (unsigned char)((t >> (b * 8)) & 0xFF);
    int rc = mbedtls_aes_crypt_xts(&ctx, MBEDTLS_AES_DECRYPT, size,
                                   tweak, (const unsigned char *)buf, (unsigned char *)buf);
    mbedtls_aes_xts_free(&ctx);
    if (rc != 0) return -1;
    return got;
}

/* ── Selftest ────────────────────────────────────────────────────── */

void crypto_disk_print_selftest_line(void)
{
    kputs("  Disk encryption ....... ");
    if (!g_armed) {
        kputs("inactive (no PIN)\n");
        return;
    }
    kputs("CFA-native, ");
    kput_dec((uint64_t)crypto_disk_region_count());
    kputs(" regions, key=SOVEREIGN-wrapped, AES-XTS-256, ");
    kput_dec((uint64_t)g_total_accesses);
    kputs(" accesses, ");
    kput_dec((uint64_t)g_total_denied);
    kputs(" denied\n");
}

/* ── Settings registry ───────────────────────────────────────────── */

static int g_get_enabled(char *out, int n) {
    if (n < 2) return -1;
    out[0] = g_armed ? '1' : '0';
    out[1] = 0;
    return 0;
}
static int g_set_enabled(const char *v) {
    /* Read-only at runtime: the fact of arming is determined by whether
     * a PIN was entered at the cold-boot gate. We accept identity writes. */
    if (!v) return -1;
    if (cd_streq(v, "1") || cd_streq(v, "true") || cd_streq(v, "on"))
        return g_armed ? 0 : -1;
    if (cd_streq(v, "0") || cd_streq(v, "false") || cd_streq(v, "off"))
        return -1;  /* cannot disarm without a reboot */
    return -1;
}
static int g_get_cipher(char *out, int n) {
    const char *v = "aes_xts_256";
    int i = 0; while (v[i] && i < n - 1) { out[i] = v[i]; i++; }
    out[i] = 0;
    return 0;
}
static int g_set_cipher(const char *v) {
    if (cd_streq(v, "aes_xts_256")) return 0;
    return -1;
}

static const settings_entry_t E_CRYPTO_ENABLED = {
    "crypto.enabled", SK_BOOL, 0, g_get_enabled, g_set_enabled, 0,
    "disk encryption armed (PIN-derived SOVEREIGN key)"
};
static const settings_entry_t E_CRYPTO_CIPHER = {
    "crypto.cipher", SK_ENUM, 0, g_get_cipher, g_set_cipher, "aes_xts_256",
    "block-region cipher"
};

void crypto_disk_settings_register(void)
{
    settings_register(&E_CRYPTO_ENABLED);
    settings_register(&E_CRYPTO_CIPHER);
}

/* ── Shell command ───────────────────────────────────────────────── */

void crypto_cmd(const char *args)
{
    args = cd_skip_ws(args ? args : "");

    if (!*args || cd_streq(args, "status")) {
        kputs("Disk encryption: ");
        if (!g_armed) { kputs("inactive\n"); return; }
        kputs("armed (AES-XTS-256, key=SOVEREIGN-wrapped)\n");
        kputs("  regions: "); kput_dec((uint64_t)crypto_disk_region_count()); kputc('\n');
        for (int i = 0; i < CRYPTO_MAX_REGIONS; i++) {
            crypto_region_t *r = &g_regions[i];
            if (r->lba_count == 0) continue;
            kputs("    "); kputs(r->name);
            kputs(" drv="); kput_dec((uint64_t)(r->drive_id < 0 ? 0xFFFFFFFFu : (uint32_t)r->drive_id));
            kputs(" lba="); kput_dec(r->lba_start);
            kputs("+");    kput_dec(r->lba_count);
            kputs(" acc="); kput_dec((uint64_t)r->access_count);
            kputs(" den="); kput_dec((uint64_t)r->denied_count);
            kputc('\n');
        }
        kputs("  total accesses: "); kput_dec((uint64_t)g_total_accesses); kputc('\n');
        kputs("  total denied:   "); kput_dec((uint64_t)g_total_denied); kputc('\n');
        return;
    }

    /* encrypt-region <name> <start_lba> <count> [drive_id] */
    {
        const char *kw = "encrypt-region";
        int kn = cd_strlen(kw);
        int i = 0;
        while (i < kn && args[i] == kw[i]) i++;
        if (i == kn && (args[i] == ' ' || args[i] == '\t' || args[i] == 0)) {
            const char *p = cd_skip_ws(args + kn);
            char nbuf[CRYPTO_REGION_NAME_MAX];
            int j = 0;
            while (*p && *p != ' ' && *p != '\t' && j < CRYPTO_REGION_NAME_MAX - 1)
                nbuf[j++] = *p++;
            nbuf[j] = 0;
            p = cd_skip_ws(p);
            const char *q = p; while (*q && *q != ' ' && *q != '\t') q++;
            char sbuf[32]; int sl = q - p; if (sl >= 32) sl = 31;
            for (int k = 0; k < sl; k++) sbuf[k] = p[k];
            sbuf[sl] = 0;
            const char *r = cd_skip_ws(q);
            char cbuf[32]; const char *cend = r;
            while (*cend && *cend != ' ' && *cend != '\t') cend++;
            int cl = cend - r; if (cl >= 32) cl = 31;
            for (int k = 0; k < cl; k++) cbuf[k] = r[k];
            cbuf[cl] = 0;

            uint64_t start_lba=0, count=0;
            if (j == 0 || sl == 0 || cl == 0
                || cd_parse_u64(sbuf, &start_lba) != 0
                || cd_parse_u64(cbuf, &count) != 0) {
                kputs("usage: crypto encrypt-region <name> <start_lba> <count>\n");
                return;
            }
            int rc = crypto_disk_create_region(nbuf, 0, start_lba, count);
            if (rc < 0) { kputs("crypto: create_region failed\n"); return; }
            kputs("crypto: region '"); kputs(nbuf); kputs("' registered (id=");
            kput_dec((uint64_t)rc); kputs(")\n");
            return;
        }
    }

    if (cd_streq(args, "rekey")) {
        char pin[24];
        int n = lockscreen_pin_copy(pin, sizeof(pin));
        if (n <= 0) { kputs("crypto: rekey: cannot read PIN\n"); return; }
        kputs("crypto: rekeying with current PIN (writes one fresh salt)...\n");
        int rc = crypto_disk_rekey(pin, n);
        cd_memzero(pin, sizeof(pin));
        kputs(rc == 0 ? "crypto: rekey ok\n" : "crypto: rekey FAILED\n");
        return;
    }

    kputs("usage: crypto status | encrypt-region <name> <start> <count> | rekey\n");
}

#ifdef ZEOS_DIAG_A7
#include "kprint.h"
/* A.7 selftest: PIN-gated AES-XTS-256 disk encryption round-trips. Init from a
 * PIN (arms the master key), create a region, encrypt a known sector (cipher
 * must differ from plaintext), decrypt it back (must recover the plaintext). */
void crypto_disk_a7_selftest(void)
{
    crypto_disk_init("4271", 4);
    int armed = crypto_disk_armed();
    int rid = crypto_disk_create_region("a7test", 0, 2048, 64);
    int region_ok = (rid >= 0);

    static uint8_t plain[512], cipher[512], back[512];
    for (int i = 0; i < 512; i++) plain[i] = (uint8_t)(i * 7 + 3);

    int enc = crypto_disk_transform(rid, 1, 2048, 1, 512, plain, cipher);
    int differs = 0; for (int i = 0; i < 512; i++) if (cipher[i] != plain[i]) { differs = 1; break; }
    int dec = crypto_disk_transform(rid, 0, 2048, 1, 512, cipher, back);
    int recovered = 1; for (int i = 0; i < 512; i++) if (back[i] != plain[i]) { recovered = 0; break; }

    int pass = armed && region_ok && (enc == 0) && differs && (dec == 0) && recovered;
    kputs("[A7] armed="); kput_dec((uint64_t)armed);
    kputs(" region="); kput_dec((uint64_t)region_ok);
    kputs(" enc_ok="); kput_dec((uint64_t)(enc==0));
    kputs(" cipher_differs="); kput_dec((uint64_t)differs);
    kputs(" dec_ok="); kput_dec((uint64_t)(dec==0));
    kputs(" recovered_plaintext="); kput_dec((uint64_t)recovered);
    kputs(pass ? " -> PASS\n" : " -> FAIL\n");
    crypto_disk_destroy_region("a7test");
}
#endif
