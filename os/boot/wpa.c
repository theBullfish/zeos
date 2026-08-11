/*
 * Zeos -- WPA2-PSK supplicant (4-way handshake)
 *
 * Pure software, no hardware dependencies. Wired into a NIC driver
 * (e.g. net_rtl8188eu.c) which feeds EAPOL frames in/out.
 *
 * IEEE 802.11i / 802.11-2020 §12.7 reference. We implement only the
 * subset required for WPA2-Personal (PSK) with CCMP:
 *   - PMK = PBKDF2-HMAC-SHA1(passphrase, ssid, 4096, 32)
 *   - PTK = PRF-384(PMK, "Pairwise key expansion",
 *                   min(AA,SPA) || max(AA,SPA) ||
 *                   min(ANonce,SNonce) || max(ANonce,SNonce))
 *   - MIC = HMAC-SHA1(KCK, EAPOL-Key frame with MIC field zeroed)[0..15]
 *   - GTK from M3 KeyData unwrapped via NIST AES Key-Wrap (RFC 3394).
 *
 * EAPOL-Key frame layout (IEEE 802.11-2020 Figure 12-32) is built
 * by hand here — keeps the module dependency-free.
 */

#include "wpa.h"
#include "kprint.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/md.h"
#include "mbedtls/sha1.h"
#include "mbedtls/nist_kw.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"

extern void *memset(void *s, int c, unsigned long n);
extern void *memcpy(void *dst, const void *src, unsigned long n);
extern int   memcmp(const void *a, const void *b, unsigned long n);

/* Provided by mbedtls_platform.c. */
extern int mbedtls_hardware_poll(void *data, unsigned char *output,
                                 size_t len, size_t *olen);

/* ─── helpers ─────────────────────────────────────────────────── */

static void min_max_6(const uint8_t *a, const uint8_t *b,
                      uint8_t *lo, uint8_t *hi)
{
    if (memcmp(a, b, 6) < 0) {
        memcpy(lo, a, 6); memcpy(hi, b, 6);
    } else {
        memcpy(lo, b, 6); memcpy(hi, a, 6);
    }
}

static void min_max_n(const uint8_t *a, const uint8_t *b, int n,
                      uint8_t *lo, uint8_t *hi)
{
    if (memcmp(a, b, (unsigned long)n) < 0) {
        memcpy(lo, a, (unsigned long)n); memcpy(hi, b, (unsigned long)n);
    } else {
        memcpy(lo, b, (unsigned long)n); memcpy(hi, a, (unsigned long)n);
    }
}

/* ─── HMAC-SHA1 wrapper ───────────────────────────────────────── */

void wpa_hmac_sha1(const uint8_t *key, int key_len,
                   const uint8_t *msg, int msg_len,
                   uint8_t out[20])
{
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    if (!md) { memset(out, 0, 20); return; }
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    if (mbedtls_md_setup(&ctx, md, 1) != 0) { mbedtls_md_free(&ctx); memset(out,0,20); return; }
    mbedtls_md_hmac_starts(&ctx, key, (size_t)key_len);
    mbedtls_md_hmac_update(&ctx, msg, (size_t)msg_len);
    mbedtls_md_hmac_finish(&ctx, out);
    mbedtls_md_free(&ctx);
}

/* ─── 802.11i PRF-N (n in bits, multiple of 8) ───────────────── */
/*
 * PRF-N(K, A, B, n):
 *   for i in 0..ceil(n/160)-1:
 *     R || = HMAC-SHA1(K, A || 0x00 || B || i)
 *   return first n/8 bytes of R.
 */
void wpa_pseudo_random_function(const uint8_t *key, int key_len,
                                const char *label,
                                const uint8_t *data, int data_len,
                                uint8_t *out, int out_len)
{
    /* Buffer: label || 0x00 || data || counter byte */
    uint8_t buf[128];
    int label_len = 0;
    while (label[label_len]) label_len++;
    if (label_len + 1 + data_len + 1 > (int)sizeof(buf)) {
        /* Should never happen for our 76-byte data inputs. */
        memset(out, 0, (unsigned long)out_len);
        return;
    }
    memcpy(buf, label, (unsigned long)label_len);
    buf[label_len] = 0x00;
    memcpy(buf + label_len + 1, data, (unsigned long)data_len);
    int total = label_len + 1 + data_len + 1;

    int produced = 0;
    uint8_t i = 0;
    uint8_t digest[20];
    while (produced < out_len) {
        buf[total - 1] = i;
        wpa_hmac_sha1(key, key_len, buf, total, digest);
        int chunk = out_len - produced;
        if (chunk > 20) chunk = 20;
        memcpy(out + produced, digest, (unsigned long)chunk);
        produced += chunk;
        i++;
    }
}

/* ─── PMK derivation ─────────────────────────────────────────── */

int wpa_derive_pmk(const char *passphrase, const char *ssid,
                   uint8_t pmk_out[WPA_PMK_LEN])
{
    int plen = 0; while (passphrase[plen]) plen++;
    int slen = 0; while (ssid[slen])       slen++;
    if (plen < 8 || plen > 63) return -1;
    if (slen < 1 || slen > 32) return -1;

    int rc = mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA1,
                                           (const unsigned char *)passphrase,
                                           (size_t)plen,
                                           (const unsigned char *)ssid,
                                           (size_t)slen,
                                           4096, WPA_PMK_LEN, pmk_out);
    return rc == 0 ? 0 : -1;
}

/* ─── PTK derivation ─────────────────────────────────────────── */

static void wpa_derive_ptk(wpa_ctx_t *ctx)
{
    /* Concat: min(AA,SPA)||max(AA,SPA)||min(NA,NS)||max(NA,NS) — 76 bytes */
    uint8_t data[6 + 6 + WPA_NONCE_LEN + WPA_NONCE_LEN];
    min_max_6(ctx->aa, ctx->spa, data, data + 6);
    min_max_n(ctx->anonce, ctx->snonce, WPA_NONCE_LEN,
              data + 12, data + 12 + WPA_NONCE_LEN);

    wpa_pseudo_random_function(ctx->pmk, WPA_PMK_LEN,
                               "Pairwise key expansion",
                               data, sizeof(data),
                               ctx->ptk, WPA_PTK_LEN);
    memcpy(ctx->kck, ctx->ptk + 0,  WPA_KCK_LEN);
    memcpy(ctx->kek, ctx->ptk + 16, WPA_KEK_LEN);
    memcpy(ctx->tk,  ctx->ptk + 32, WPA_TK_LEN);
}

/* ─── EAPOL-Key frame layout ─────────────────────────────────── */
/*
 *  off  size  field
 *    0     1  EAPOL Version       (0x02 for 802.11i)
 *    1     1  EAPOL Type          (0x03 = EAPOL-Key)
 *    2     2  EAPOL Body Length   (big-endian, bytes after this field)
 *    4     1  Descriptor Type     (0x02 = RSN/WPA2)
 *    5     2  Key Information     (big-endian)
 *    7     2  Key Length          (big-endian) — 16 for CCMP pairwise, 0 in M2/M4
 *    9     8  Key Replay Counter  (big-endian)
 *   17    32  Key Nonce
 *   49    16  EAPOL-Key IV        (zero for CCMP)
 *   65     8  Key RSC
 *   73     8  Reserved
 *   81    16  Key MIC             (HMAC-SHA1-128, computed with this zeroed)
 *   97     2  Key Data Length     (big-endian)
 *   99   var  Key Data            (RSN IE in M2; wrapped GTK in M3)
 *
 * Total fixed = 99 bytes.
 */
#define EAPOL_OFF_VER       0
#define EAPOL_OFF_TYPE      1
#define EAPOL_OFF_BODYLEN   2   /* 2 bytes BE */
#define EAPOL_OFF_DESC      4
#define EAPOL_OFF_KEYINFO   5   /* 2 BE */
#define EAPOL_OFF_KEYLEN    7   /* 2 BE */
#define EAPOL_OFF_REPLAY    9   /* 8 BE */
#define EAPOL_OFF_NONCE    17   /* 32 */
#define EAPOL_OFF_KEYIV    49   /* 16 */
#define EAPOL_OFF_KEYRSC   65   /* 8 */
#define EAPOL_OFF_RESV     73   /* 8 */
#define EAPOL_OFF_MIC      81   /* 16 */
#define EAPOL_OFF_KDLEN    97   /* 2 BE */
#define EAPOL_OFF_KEYDATA  99
#define EAPOL_FIXED        99

/* Key Information bits (IEEE 802.11-2020 Table 12-9) */
#define KI_VER_MASK         0x0007
#define KI_VER_HMAC_SHA1    0x0002   /* CCMP/AES-CMAC negotiated */
#define KI_TYPE_PAIRWISE    0x0008
#define KI_INSTALL          0x0040
#define KI_ACK              0x0080
#define KI_MIC              0x0100
#define KI_SECURE           0x0200
#define KI_ERROR            0x0400
#define KI_REQ              0x0800
#define KI_ENCRYPTED_KD     0x1000

static uint16_t rd_be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static void     wr_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

/* ─── RSN IE builder ─────────────────────────────────────────── */
/*
 * 22-byte RSN IE for WPA2-PSK + CCMP pairwise + CCMP group:
 *   30  ID
 *   14  Length (=20)
 *   01 00            Version
 *   00 0F AC 04      Group cipher: CCMP (00-0F-AC:4)
 *   01 00            Pairwise count = 1
 *   00 0F AC 04      Pairwise: CCMP
 *   01 00            AKM count = 1
 *   00 0F AC 02      AKM: PSK (00-0F-AC:2)
 *   00 00            RSN capabilities
 */
int wpa_build_rsn_ie(uint8_t *out, int out_max)
{
    static const uint8_t ie[22] = {
        0x30, 0x14,
        0x01, 0x00,
        0x00, 0x0F, 0xAC, 0x04,
        0x01, 0x00,
        0x00, 0x0F, 0xAC, 0x04,
        0x01, 0x00,
        0x00, 0x0F, 0xAC, 0x02,
        0x00, 0x00
    };
    if (out_max < (int)sizeof(ie)) return -1;
    memcpy(out, ie, sizeof(ie));
    return (int)sizeof(ie);
}

/* ─── handshake start ────────────────────────────────────────── */

static int random_bytes(uint8_t *out, int n)
{
    size_t got = 0;
    if (mbedtls_hardware_poll(0, out, (size_t)n, &got) != 0) return -1;
    return got == (size_t)n ? 0 : -1;
}

int wpa_start(wpa_ctx_t *ctx, const uint8_t spa[6], const uint8_t aa[6])
{
    if (!ctx || !spa || !aa) return -1;

    /* Preserve PMK (caller already populated). Wipe everything else. */
    uint8_t saved_pmk[WPA_PMK_LEN];
    memcpy(saved_pmk, ctx->pmk, WPA_PMK_LEN);
    memset(ctx, 0, sizeof(*ctx));
    memcpy(ctx->pmk, saved_pmk, WPA_PMK_LEN);

    memcpy(ctx->spa, spa, 6);
    memcpy(ctx->aa,  aa,  6);
    if (random_bytes(ctx->snonce, WPA_NONCE_LEN) < 0) {
        ctx->state = WPA_STATE_FAILED;
        return -1;
    }
    ctx->state = WPA_STATE_WAIT_M1;
    return 0;
}

/* ─── EAPOL handler (frames 1 and 3) ─────────────────────────── */

static int build_m2(wpa_ctx_t *ctx, uint8_t *out, int out_max, int *out_len);
static int build_m4(wpa_ctx_t *ctx, const uint8_t *m3, int m3_len,
                    uint8_t *out, int out_max, int *out_len);

int wpa_handle_eapol(wpa_ctx_t *ctx,
                     const uint8_t *eapol, int len,
                     uint8_t *out_reply, int out_max,
                     int *out_reply_len)
{
    if (out_reply_len) *out_reply_len = 0;
    if (!ctx || !eapol || len < EAPOL_FIXED) return -1;

    if (eapol[EAPOL_OFF_TYPE] != 0x03) return -1;       /* must be EAPOL-Key */
    if (eapol[EAPOL_OFF_DESC] != 0x02) return -1;       /* must be RSN */

    uint16_t ki = rd_be16(eapol + EAPOL_OFF_KEYINFO);
    int has_mic       = (ki & KI_MIC) != 0;
    int is_pairwise   = (ki & KI_TYPE_PAIRWISE) != 0;
    int has_ack       = (ki & KI_ACK) != 0;
    int has_install   = (ki & KI_INSTALL) != 0;

    /* Frame 1: pairwise + ACK, no MIC. */
    if (ctx->state == WPA_STATE_WAIT_M1 &&
        is_pairwise && has_ack && !has_mic && !has_install)
    {
        memcpy(ctx->anonce, eapol + EAPOL_OFF_NONCE, WPA_NONCE_LEN);
        memcpy(ctx->replay_counter, eapol + EAPOL_OFF_REPLAY, 8);
        wpa_derive_ptk(ctx);
        if (build_m2(ctx, out_reply, out_max, out_reply_len) < 0) {
            ctx->state = WPA_STATE_FAILED;
            return -1;
        }
        ctx->state = WPA_STATE_SENT_M2;
        return 0;
    }

    /* Frame 3: pairwise + MIC + ACK + INSTALL. Replay counter must
     * be strictly greater than the one we recorded for M1. */
    if (ctx->state == WPA_STATE_SENT_M2 &&
        is_pairwise && has_mic && has_ack && has_install)
    {
        if (memcmp(eapol + EAPOL_OFF_REPLAY, ctx->replay_counter, 8) <= 0)
            return -1;

        /* Verify MIC: clear MIC field, recompute, compare. */
        uint8_t copy[512];
        if (len > (int)sizeof(copy)) return -1;
        memcpy(copy, eapol, (unsigned long)len);
        memset(copy + EAPOL_OFF_MIC, 0, WPA_MIC_LEN);
        uint8_t mic[20];
        wpa_hmac_sha1(ctx->kck, WPA_KCK_LEN, copy, len, mic);
        if (memcmp(mic, eapol + EAPOL_OFF_MIC, WPA_MIC_LEN) != 0)
            return -1;

        /* Update replay counter to M3's value (we'll echo it in M4). */
        memcpy(ctx->replay_counter, eapol + EAPOL_OFF_REPLAY, 8);

        /* Pull GTK from KeyData. If ENCRYPTED_KD bit set, KeyData is
         * AES-Key-Wrapped under KEK (RFC 3394). KeyData payload is a
         * sequence of KDEs (Key Data Elements) — we look for a GTK KDE
         * with selector 00-0F-AC-01. */
        int kdlen = rd_be16(eapol + EAPOL_OFF_KDLEN);
        if (kdlen < 0 || EAPOL_OFF_KEYDATA + kdlen > len) return -1;

        uint8_t plain[256];
        int plain_len = 0;

        if (ki & KI_ENCRYPTED_KD) {
            if (kdlen < 16 || kdlen > (int)sizeof(plain) + 8) return -1;
            mbedtls_nist_kw_context kwc;
            mbedtls_nist_kw_init(&kwc);
            if (mbedtls_nist_kw_setkey(&kwc, MBEDTLS_CIPHER_ID_AES,
                                       ctx->kek, WPA_KEK_LEN * 8, 0) != 0) {
                mbedtls_nist_kw_free(&kwc);
                return -1;
            }
            size_t out_n = 0;
            int rc = mbedtls_nist_kw_unwrap(&kwc, MBEDTLS_KW_MODE_KW,
                                            eapol + EAPOL_OFF_KEYDATA,
                                            (size_t)kdlen,
                                            plain, &out_n, sizeof(plain));
            mbedtls_nist_kw_free(&kwc);
            if (rc != 0) return -1;
            plain_len = (int)out_n;
        } else {
            if (kdlen > (int)sizeof(plain)) return -1;
            memcpy(plain, eapol + EAPOL_OFF_KEYDATA, (unsigned long)kdlen);
            plain_len = kdlen;
        }

        /* Walk KDEs: each is { 0xDD, len, OUI(3), DataType, Body } */
        int i = 0;
        while (i + 2 <= plain_len) {
            uint8_t tag  = plain[i];
            uint8_t tlen = plain[i + 1];
            if (i + 2 + tlen > plain_len) break;
            if (tag == 0xDD && tlen >= 6 &&
                plain[i+2] == 0x00 && plain[i+3] == 0x0F &&
                plain[i+4] == 0xAC && plain[i+5] == 0x01)
            {
                /* GTK KDE: byte0 = key_id (bits 0..1) + tx (bit 2),
                 * byte1 reserved, body follows. */
                int key_id = plain[i+6] & 0x03;
                int gtk_off = i + 8;
                int gtk_len = tlen - 6;   /* tlen excludes the 0xDD,len bytes */
                if (gtk_len < 0 || gtk_len > WPA_GTK_MAX) return -1;
                memcpy(ctx->gtk, plain + gtk_off, (unsigned long)gtk_len);
                ctx->gtk_len    = gtk_len;
                ctx->gtk_keyidx = key_id;
            }
            i += 2 + tlen;
        }

        if (build_m4(ctx, eapol, len, out_reply, out_max, out_reply_len) < 0) {
            ctx->state = WPA_STATE_FAILED;
            return -1;
        }
        ctx->state         = WPA_STATE_DONE;
        ctx->ptk_installed = 1;
        ctx->gtk_installed = ctx->gtk_len > 0 ? 1 : 0;
        return 0;
    }

    return -1;
}

/* ─── M2 builder: SNonce + RSN IE + MIC ──────────────────────── */

static int build_m2(wpa_ctx_t *ctx, uint8_t *out, int out_max, int *out_len)
{
    uint8_t rsn[32];
    int rsn_len = wpa_build_rsn_ie(rsn, sizeof(rsn));
    if (rsn_len < 0) return -1;

    int total = EAPOL_FIXED + rsn_len;
    if (total > out_max) return -1;
    memset(out, 0, (unsigned long)total);

    out[EAPOL_OFF_VER]  = 0x02;
    out[EAPOL_OFF_TYPE] = 0x03;
    wr_be16(out + EAPOL_OFF_BODYLEN, (uint16_t)(total - 4));
    out[EAPOL_OFF_DESC] = 0x02;
    /* Key Info: pairwise + MIC + version=HMAC-SHA1 */
    wr_be16(out + EAPOL_OFF_KEYINFO,
            (uint16_t)(KI_VER_HMAC_SHA1 | KI_TYPE_PAIRWISE | KI_MIC));
    wr_be16(out + EAPOL_OFF_KEYLEN, 0);
    memcpy(out + EAPOL_OFF_REPLAY, ctx->replay_counter, 8);
    memcpy(out + EAPOL_OFF_NONCE,  ctx->snonce, WPA_NONCE_LEN);
    wr_be16(out + EAPOL_OFF_KDLEN, (uint16_t)rsn_len);
    memcpy(out + EAPOL_OFF_KEYDATA, rsn, (unsigned long)rsn_len);

    /* MIC: HMAC-SHA1(KCK, frame with MIC zeroed)[0..15] */
    uint8_t mic[20];
    wpa_hmac_sha1(ctx->kck, WPA_KCK_LEN, out, total, mic);
    memcpy(out + EAPOL_OFF_MIC, mic, WPA_MIC_LEN);

    *out_len = total;
    return 0;
}

/* ─── M4 builder: ACK with MIC, no key data ──────────────────── */

static int build_m4(wpa_ctx_t *ctx, const uint8_t *m3, int m3_len,
                    uint8_t *out, int out_max, int *out_len)
{
    (void)m3; (void)m3_len;
    int total = EAPOL_FIXED;
    if (total > out_max) return -1;
    memset(out, 0, (unsigned long)total);

    out[EAPOL_OFF_VER]  = 0x02;
    out[EAPOL_OFF_TYPE] = 0x03;
    wr_be16(out + EAPOL_OFF_BODYLEN, (uint16_t)(total - 4));
    out[EAPOL_OFF_DESC] = 0x02;
    /* Key Info: pairwise + MIC + SECURE + version=HMAC-SHA1 */
    wr_be16(out + EAPOL_OFF_KEYINFO,
            (uint16_t)(KI_VER_HMAC_SHA1 | KI_TYPE_PAIRWISE |
                       KI_MIC | KI_SECURE));
    wr_be16(out + EAPOL_OFF_KEYLEN, 0);
    memcpy(out + EAPOL_OFF_REPLAY, ctx->replay_counter, 8);
    /* Nonce all-zero, KeyData empty. */
    wr_be16(out + EAPOL_OFF_KDLEN, 0);

    uint8_t mic[20];
    wpa_hmac_sha1(ctx->kck, WPA_KCK_LEN, out, total, mic);
    memcpy(out + EAPOL_OFF_MIC, mic, WPA_MIC_LEN);

    *out_len = total;
    return 0;
}
