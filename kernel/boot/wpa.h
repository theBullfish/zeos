/*
 * Zeos -- WPA2-PSK supplicant
 *
 * Pure-software state machine for the IEEE 802.11i 4-way handshake.
 * No hardware dependencies; the caller (e.g. net_rtl8188eu.c) is
 * responsible for shipping EAPOL frames out the chip and feeding
 * received EAPOL frames in.
 *
 * Cryptographic primitives come from mbedTLS (PBKDF2-HMAC-SHA1 for
 * the PMK, HMAC-SHA1 for the PTK PRF and MIC, AES-128 unwrap for
 * GTK delivery in Frame 3).
 *
 * Reference: IEEE 802.11-2020, sections 12.7 (RSNA key management)
 * and 4.10.4.2 (4-way handshake).
 */

#ifndef ZEOS_WPA_H
#define ZEOS_WPA_H

#include <stdint.h>
#include <stddef.h>

#define WPA_PMK_LEN     32   /* WPA2 PMK */
#define WPA_PTK_LEN     48   /* TKIP-free; CCMP PTK is 48: 16 KCK || 16 KEK || 16 TK */
#define WPA_KCK_LEN     16
#define WPA_KEK_LEN     16
#define WPA_TK_LEN      16
#define WPA_MIC_LEN     16
#define WPA_NONCE_LEN   32
#define WPA_GTK_MAX     32

/* Supplicant state machine. */
typedef enum {
    WPA_STATE_IDLE      = 0,
    WPA_STATE_WAIT_M1   = 1,  /* waiting for AP's first EAPOL */
    WPA_STATE_SENT_M2   = 2,  /* sent SNonce + MIC */
    WPA_STATE_WAIT_M3   = 3,  /* waiting for GTK + MIC */
    WPA_STATE_SENT_M4   = 4,  /* sent ack */
    WPA_STATE_DONE      = 5,  /* PTK + GTK installed */
    WPA_STATE_FAILED    = 6
} wpa_state_t;

/* Supplicant context. One per association. */
typedef struct {
    wpa_state_t state;

    /* Long-term: PSK-derived PMK (32 bytes). */
    uint8_t  pmk[WPA_PMK_LEN];

    /* Per-handshake. */
    uint8_t  anonce[WPA_NONCE_LEN];   /* from AP, frame 1 */
    uint8_t  snonce[WPA_NONCE_LEN];   /* ours */
    uint8_t  aa[6];                   /* authenticator (AP) MAC */
    uint8_t  spa[6];                  /* supplicant (us) MAC */

    /* Derived keys. */
    uint8_t  ptk[WPA_PTK_LEN];
    uint8_t  kck[WPA_KCK_LEN];        /* PTK[0..15]   — Key Confirmation Key */
    uint8_t  kek[WPA_KEK_LEN];        /* PTK[16..31]  — Key Encryption Key */
    uint8_t  tk[WPA_TK_LEN];          /* PTK[32..47]  — Temporal Key (CCMP) */

    uint8_t  gtk[WPA_GTK_MAX];
    int      gtk_len;
    int      gtk_keyidx;

    /* Replay counter — must echo what AP sent in M1/M3. */
    uint8_t  replay_counter[8];

    /* Bookkeeping. */
    int      ptk_installed;
    int      gtk_installed;
} wpa_ctx_t;

/*
 * PMK = PBKDF2-HMAC-SHA1(passphrase, ssid, 4096, 32).
 * Returns 0 on success.
 */
int wpa_derive_pmk(const char *passphrase, const char *ssid,
                   uint8_t pmk_out[WPA_PMK_LEN]);

/*
 * Reset and start a new handshake. SPA = our MAC, AA = AP BSSID.
 * Caller must have placed the PSK-derived PMK in ctx->pmk.
 * Generates a random SNonce.
 * Returns 0.
 */
int wpa_start(wpa_ctx_t *ctx, const uint8_t spa[6], const uint8_t aa[6]);

/*
 * Feed a received EAPOL-Key frame (from AP) into the state machine.
 * `eapol` points at the EAPOL header (LLC/SNAP and 802.11 already
 * stripped; the buffer starts with EAPOL Version byte 0x01 or 0x02).
 *
 * On success, *out_reply / *out_reply_len are populated with the
 * reply EAPOL-Key frame the caller should ship to the AP. If no
 * reply is required, *out_reply_len is set to 0.
 *
 * Returns 0 if the frame was consumed cleanly (handshake advanced
 * or completed), -1 if it was malformed / MIC failed / replay was
 * stale / state mismatch.
 */
int wpa_handle_eapol(wpa_ctx_t *ctx,
                     const uint8_t *eapol, int len,
                     uint8_t *out_reply, int out_max,
                     int *out_reply_len);

/* Convenience: raw 4-way PRF used internally; exposed so the driver
 * can verify our test vectors during selftest. */
void wpa_pseudo_random_function(const uint8_t *key, int key_len,
                                const char *label,
                                const uint8_t *data, int data_len,
                                uint8_t *out, int out_len);

/* HMAC-SHA1 wrapper used for MIC validation. */
void wpa_hmac_sha1(const uint8_t *key, int key_len,
                   const uint8_t *msg, int msg_len,
                   uint8_t out[20]);

/* Build the 22-byte WPA2 RSN IE that goes into the (re)association
 * request. CCMP pairwise + group, PSK AKM. Returns the IE length. */
int wpa_build_rsn_ie(uint8_t *out, int out_max);

#endif /* ZEOS_WPA_H */
