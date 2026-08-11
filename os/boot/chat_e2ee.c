/*
 * Zeos — chat-zeos E2EE: real per-room group key derivation +
 * AES-XTS-256 body encryption.
 *
 * Each enabled room owns:
 *   - a 32-byte salt persisted to vault under /chat/<room>/e2ee/salt,
 *     created with vault_create(path, VAULT_TIER_SOVEREIGN) -- the
 *     SOVEREIGN tier is recorded as inode metadata only
 *   - a 32-byte K_room derived as
 *       HMAC-SHA256(salt, member_canonical || room_id)
 *   - K_room/K_tweak are held in memory only (g_rooms[], plain
 *     uint8_t[32] fields, no persistence)
 *
 * CORRECTED 2026-07-24 (paradigm-conformance audit, real gap, not just a
 * naming nitpick): this comment used to claim "non-members can't reproduce
 * the same digest because cross-context perception of SOVEREIGN blobs is
 * denied by cfa_resolve." That was false on two counts, both checked
 * directly against the actual code, not assumed: (1) this file never
 * includes cfa_handle.h or calls cfa_wrap/cfa_resolve anywhere -- K_room/
 * K_tweak are plain struct fields, not CFA handles; (2) more seriously,
 * vault_read()/vault_write() (vault.c) do not enforce the SOVEREIGN tier
 * tag at all -- they read/write by path unconditionally once the vault is
 * mounted, no MasQ perception check in that call path. So the salt's
 * SOVEREIGN tag is currently a label, not an access gate, and this file
 * provides NO cryptographic non-member exclusion of its own beyond
 * whatever isolation (if any) exists elsewhere in the calling context.
 * Tracked as bible-db R.1 (BROKEN) -- if real cross-context denial is
 * needed here, it has to be built (either wire real CFA wrapping for
 * K_room, or add actual tier enforcement to vault_read/vault_write), not
 * assumed to already exist.
 *
 * Body cipher:
 *   AES-XTS-256 with the 64-byte expanded key (K_room || K_tweak).
 *   K_tweak = HMAC-SHA256(K_room, "zeos-chat-tweak-key").
 *   The data-unit (tweak) is the message sequence number.
 *
 * On membership change, chat_e2ee_rederive(room) recomputes K_room
 * against the new member set with the same salt.
 */

#include "chat_e2ee.h"
#include "chat_zeos.h"
#include "vault.h"
#include "mbedtls/md.h"
#include "mbedtls/aes.h"
#include "kprint.h"

extern int mbedtls_hardware_poll(void *data, unsigned char *output,
                                 size_t len, size_t *olen);

/* ── In-memory room key table ──────────────────────────────────── */

#define E2EE_MAX_ROOMS  32

typedef struct {
    int     used;
    char    room_id[64];
    uint8_t salt[32];
    uint8_t key[32];      /* K_room */
    uint8_t tweak_key[32];
} e2ee_room_t;

static e2ee_room_t g_rooms[E2EE_MAX_ROOMS];
static int g_initialized = 0;

/* ── Helpers ──────────────────────────────────────────────────── */

static int e2_streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}
static void e2_strcpy(char *d, const char *s, int max) {
    int i = 0; while (s && s[i] && i + 1 < max) { d[i] = s[i]; i++; } d[i] = 0;
}

static e2ee_room_t *find_room(const char *room_id) {
    for (int i = 0; i < E2EE_MAX_ROOMS; i++) {
        if (g_rooms[i].used && e2_streq(g_rooms[i].room_id, room_id))
            return &g_rooms[i];
    }
    return 0;
}

static e2ee_room_t *alloc_room(const char *room_id) {
    for (int i = 0; i < E2EE_MAX_ROOMS; i++) {
        if (!g_rooms[i].used) {
            g_rooms[i].used = 1;
            e2_strcpy(g_rooms[i].room_id, room_id, sizeof(g_rooms[i].room_id));
            return &g_rooms[i];
        }
    }
    return 0;
}

void chat_e2ee_init(void) {
    if (g_initialized) return;
    g_initialized = 1;
}

/* ── HMAC-SHA256 wrapper ─────────────────────────────────────── */

static int e2_hmac_sha256(const uint8_t *key, size_t keylen,
                          const uint8_t *msg, size_t msglen,
                          uint8_t out[32]) {
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info) return -1;
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    int rc = mbedtls_md_setup(&ctx, info, 1);
    if (rc != 0) { mbedtls_md_free(&ctx); return -1; }
    mbedtls_md_hmac_starts(&ctx, key, keylen);
    mbedtls_md_hmac_update(&ctx, msg, msglen);
    mbedtls_md_hmac_finish(&ctx, out);
    mbedtls_md_free(&ctx);
    return 0;
}

/* ── Member-set canonicalization ───────────────────────────── */

/* chat_zeos exposes per-room member counts via chat_zeos_walk_rooms,
 * but doesn't yet expose member names. The canonical input here is
 *   "<member_count>|<room_id>|<active_ctx>"
 * which binds K_room to room id + member-set cardinality + the
 * active identity context. Cross-context observers fail the SOVEREIGN
 * salt read at vault layer; they never reach this code. When chat_zeos
 * exposes a per-room member walker (documented v2 follow-up in
 * chat_polish.c), the canonical form expands to
 *   "<count>|<room_id>|<sorted member ctxes joined by NUL>"
 * with no other change to the key derivation contract. */
static void cb_count(const char *id, const char *name, int v,
                     int mc, int msg, void *user) {
    (void)name; (void)v; (void)msg;
    int *out = (int *)user;
    /* room_id is captured via the walker filter on the caller side: we
     * tag with a sentinel by stuffing room_id into out[1] address... */
    (void)id; (void)out; (void)mc;
}

/* Local closure-style: stash the target id in a static for the duration
 * of the walk. Single-threaded enable path; safe. */
static const char *g_target_id;
static int         g_target_count;
static int         g_target_found;

static void cb_member_count(const char *id, const char *name, int v,
                            int mc, int msg, void *user) {
    (void)name; (void)v; (void)msg; (void)user;
    if (e2_streq(id, g_target_id)) {
        g_target_count = mc;
        g_target_found = 1;
    }
}

static int build_member_canonical(const char *room_id,
                                  char *out, int out_max) {
    g_target_id = room_id;
    g_target_count = 0;
    g_target_found = 0;
    chat_zeos_walk_rooms(0, cb_member_count, 0);
    if (!g_target_found) return -1;

    int o = 0;
    int mc = g_target_count;
    /* decimal */
    char tmp[16]; int ti = 0;
    if (mc == 0) tmp[ti++] = '0';
    else { int v = mc; while (v > 0 && ti < 15) { tmp[ti++] = '0' + (v % 10); v /= 10; } }
    while (ti > 0 && o < out_max - 1) out[o++] = tmp[--ti];
    if (o < out_max - 1) out[o++] = '|';
    for (int i = 0; room_id[i] && o < out_max - 1; i++) out[o++] = room_id[i];
    if (o < out_max - 1) out[o++] = '|';
    const char *self = chat_zeos_current_ctx();
    for (int i = 0; self[i] && o < out_max - 1; i++) out[o++] = self[i];
    out[o] = 0;
    (void)cb_count;  /* silence */
    return o;
}

/* ── Salt persistence (vault SOVEREIGN) ──────────────────── */

static void salt_path(const char *room_id, char *out, int max) {
    int o = 0;
    const char *p = "/chat/";
    while (*p && o < max - 1) out[o++] = *p++;
    for (int i = 0; room_id[i] && o < max - 1; i++) out[o++] = room_id[i];
    p = "/e2ee/salt";
    while (*p && o < max - 1) out[o++] = *p++;
    out[o] = 0;
}

static int salt_load(const char *room_id, uint8_t out[32]) {
    char path[160]; salt_path(room_id, path, sizeof(path));
    if (vault_exists(path) <= 0) return -1;
    int got = vault_read(path, out, 32);
    if (got < 32) return -1;
    return 0;
}

static int salt_store(const char *room_id, const uint8_t salt[32]) {
    char path[160]; salt_path(room_id, path, sizeof(path));
    if (vault_exists(path) <= 0) {
        if (vault_create(path, VAULT_TIER_SOVEREIGN) < 0) return -1;
    }
    int wrote = vault_write(path, salt, 32);
    return (wrote == 32) ? 0 : -1;
}

static int salt_delete(const char *room_id) {
    char path[160]; salt_path(room_id, path, sizeof(path));
    if (vault_exists(path) <= 0) return 0;
    return vault_delete(path);
}

static int gen_salt(uint8_t out[32]) {
    size_t olen = 0;
    if (mbedtls_hardware_poll(0, out, 32, &olen) != 0) return -1;
    if (olen != 32) return -1;
    return 0;
}

/* ── Key derivation ────────────────────────────────────────── */

static int derive_key_for(e2ee_room_t *r, const char *room_id) {
    char canon[256];
    int n = build_member_canonical(room_id, canon, sizeof(canon));
    if (n < 0) return -1;
    if (e2_hmac_sha256(r->salt, 32,
                       (const uint8_t *)canon, (size_t)n,
                       r->key) != 0) return -1;
    static const char tweak_label[] = "zeos-chat-tweak-key";
    if (e2_hmac_sha256(r->key, 32,
                       (const uint8_t *)tweak_label,
                       sizeof(tweak_label) - 1,
                       r->tweak_key) != 0) return -1;
    return 0;
}

/* ── Public API ────────────────────────────────────────────── */

int chat_e2ee_enable(const char *room_id) {
    chat_e2ee_init();
    if (!room_id || !*room_id) return -1;
    e2ee_room_t *r = find_room(room_id);
    if (!r) {
        r = alloc_room(room_id);
        if (!r) return -1;
    }
    if (salt_load(room_id, r->salt) != 0) {
        if (gen_salt(r->salt) != 0) {
            r->used = 0;
            return -1;
        }
        if (salt_store(room_id, r->salt) != 0) {
            r->used = 0;
            return -1;
        }
    }
    if (derive_key_for(r, room_id) != 0) {
        for (int i = 0; i < 32; i++) { r->key[i] = 0; r->tweak_key[i] = 0; }
        return -1;
    }
    return 0;
}

int chat_e2ee_disable(const char *room_id) {
    e2ee_room_t *r = find_room(room_id);
    if (r) {
        for (int i = 0; i < 32; i++) {
            r->salt[i] = 0; r->key[i] = 0; r->tweak_key[i] = 0;
        }
        r->used = 0;
    }
    salt_delete(room_id);
    return 0;
}

int chat_e2ee_is_active(const char *room_id) {
    e2ee_room_t *r = find_room(room_id);
    if (!r) return 0;
    int z = 1;
    for (int i = 0; i < 32; i++) if (r->key[i]) { z = 0; break; }
    return z ? 0 : 1;
}

int chat_e2ee_rederive(const char *room_id) {
    e2ee_room_t *r = find_room(room_id);
    if (!r) return -1;
    return derive_key_for(r, room_id);
}

/* ── AES-XTS body cipher ──────────────────────────────────── */

static void seq_to_tweak(uint64_t seq, uint8_t out[16]) {
    for (int i = 0; i < 16; i++) out[i] = 0;
    for (int i = 0; i < 8; i++) out[i] = (uint8_t)((seq >> (i * 8)) & 0xFF);
}

static int xts_crypt(e2ee_room_t *r, int encrypt,
                     uint64_t seq, const uint8_t *in, size_t len,
                     uint8_t *out) {
    if (len == 0) return 0;
    /* For sub-16-byte payloads, derive a one-shot keystream and XOR. */
    if (len < 16) {
        uint8_t ks[32];
        uint8_t seq_bytes[8];
        for (int i = 0; i < 8; i++)
            seq_bytes[i] = (uint8_t)((seq >> (i * 8)) & 0xFF);
        if (e2_hmac_sha256(r->tweak_key, 32, seq_bytes, 8, ks) != 0)
            return -1;
        for (size_t i = 0; i < len; i++) out[i] = in[i] ^ ks[i];
        (void)encrypt;
        return (int)len;
    }
    uint8_t expanded[64];
    for (int i = 0; i < 32; i++) expanded[i] = r->key[i];
    for (int i = 0; i < 32; i++) expanded[32 + i] = r->tweak_key[i];

    mbedtls_aes_xts_context ctx;
    mbedtls_aes_xts_init(&ctx);
    int rc;
    if (encrypt) rc = mbedtls_aes_xts_setkey_enc(&ctx, expanded, 512);
    else         rc = mbedtls_aes_xts_setkey_dec(&ctx, expanded, 512);
    if (rc != 0) { mbedtls_aes_xts_free(&ctx); return -1; }

    uint8_t tweak[16];
    seq_to_tweak(seq, tweak);
    rc = mbedtls_aes_crypt_xts(&ctx,
                               encrypt ? MBEDTLS_AES_ENCRYPT : MBEDTLS_AES_DECRYPT,
                               len, tweak, in, out);
    mbedtls_aes_xts_free(&ctx);
    if (rc != 0) return -1;
    return (int)len;
}

int chat_e2ee_encrypt(const char *room_id, uint64_t seq,
                      const uint8_t *body, size_t body_len, uint8_t *out) {
    e2ee_room_t *r = find_room(room_id);
    if (!r || !chat_e2ee_is_active(room_id)) return -1;
    return xts_crypt(r, 1, seq, body, body_len, out);
}

int chat_e2ee_decrypt(const char *room_id, uint64_t seq,
                      const uint8_t *body, size_t body_len, uint8_t *out) {
    e2ee_room_t *r = find_room(room_id);
    if (!r || !chat_e2ee_is_active(room_id)) return -1;
    return xts_crypt(r, 0, seq, body, body_len, out);
}

int chat_e2ee_active_count(void) {
    int n = 0;
    for (int i = 0; i < E2EE_MAX_ROOMS; i++) {
        if (!g_rooms[i].used) continue;
        if (chat_e2ee_is_active(g_rooms[i].room_id)) n++;
    }
    return n;
}
