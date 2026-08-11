/*
 * Zeos — mbedTLS Configuration
 *
 * Minimal config for bare-metal TLS 1.3 client.
 * No POSIX, no filesystem, no threads, no stdio.
 * Memory from heap.c, entropy from TSC, I/O from our TCP stack.
 *
 * Original file Copyright The Mbed TLS Contributors
 * SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
 */

#ifndef MBEDTLS_CONFIG_H
#define MBEDTLS_CONFIG_H

#define MBEDTLS_CONFIG_VERSION 0x03000000

/* ── Platform: bare-metal ── */
#define MBEDTLS_NO_PLATFORM_ENTROPY    /* We provide our own (TSC) */
#define MBEDTLS_ENTROPY_HARDWARE_ALT   /* We implement the HW entropy hook */
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY        /* Custom malloc/free */
#define MBEDTLS_PLATFORM_NO_STD_FUNCTIONS
#define MBEDTLS_HAVE_TIME               /* enables time-of-day calls */
#define MBEDTLS_HAVE_TIME_DATE          /* enable cert validity checks */
#define MBEDTLS_PLATFORM_TIME_ALT        /* registered via set_time() at init */
#define MBEDTLS_PLATFORM_SNPRINTF_ALT     /* Registered at runtime via mbedtls_platform_set_snprintf() */
#define MBEDTLS_PLATFORM_VSNPRINTF_ALT

/* ── Crypto primitives ── */
#define MBEDTLS_AES_C
#define MBEDTLS_CIPHER_MODE_XTS        /* AES-XTS for CFA-native disk encryption */
#define MBEDTLS_GCM_C                  /* AES-GCM (TLS 1.3 cipher) */
#define MBEDTLS_CCM_C
#define MBEDTLS_SHA1_C                 /* WPA2 4-way handshake (HMAC-SHA1) */
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_MD_C
#define MBEDTLS_PKCS5_C                /* WPA2 PSK→PMK derivation (PBKDF2-HMAC) */
#define MBEDTLS_NIST_KW_C              /* WPA2 GTK delivery (RFC 3394 unwrap) */
#define MBEDTLS_HKDF_C                 /* TLS 1.3 key derivation */
#define MBEDTLS_CIPHER_C
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_OID_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_PEM_PARSE_C

/* ── Elliptic curves (TLS 1.3 key exchange) ── */
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED  /* P-256 — mandatory for TLS 1.3 */
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED  /* P-384 */
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED /* X25519 — fast, modern */

/* ── RSA (for legacy certs) ── */
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_X509_RSASSA_PSS_SUPPORT     /* enables rsa_pss_rsae_sha* in sig_algs */
#define MBEDTLS_KEY_EXCHANGE_ECDSA_CERT_REQ_ALLOWED_ENABLED   /* TLS 1.2 ECDSA sigs */

/* ── Public key infrastructure ── */
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_X509_CRT_PARSE_C      /* Certificate parsing */
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRL_PARSE_C

/* ── RNG ── */
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_ENTROPY_C

/* ── PSA crypto (required for TLS 1.3 in mbedTLS 3.6) ── */
#define MBEDTLS_PSA_CRYPTO_C
#define MBEDTLS_USE_PSA_CRYPTO
#define PSA_WANT_ALG_HKDF 1
#define PSA_WANT_ALG_HKDF_EXTRACT 1
#define PSA_WANT_ALG_HKDF_EXPAND 1
#define PSA_WANT_ALG_SHA_256 1
#define PSA_WANT_ALG_SHA_384 1
#define PSA_WANT_ALG_SHA_512 1
#define PSA_WANT_ALG_ECDH 1
#define PSA_WANT_ALG_ECDSA 1
#define PSA_WANT_ALG_RSA_PKCS1V15_SIGN 1
#define PSA_WANT_ALG_RSA_PSS 1
#define PSA_WANT_ALG_GCM 1
#define PSA_WANT_ALG_CCM 1
#define PSA_WANT_KEY_TYPE_ECC_PUBLIC_KEY 1
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_BASIC 1
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_IMPORT 1
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_EXPORT 1
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_GENERATE 1
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_DERIVE 1
#define PSA_WANT_KEY_TYPE_RSA_PUBLIC_KEY 1
#define PSA_WANT_KEY_TYPE_RSA_KEY_PAIR_BASIC 1
#define PSA_WANT_KEY_TYPE_RSA_KEY_PAIR_IMPORT 1
#define PSA_WANT_KEY_TYPE_AES 1
#define PSA_WANT_ECC_SECP_R1_256 1
#define PSA_WANT_ECC_SECP_R1_384 1
#define PSA_WANT_ECC_MONTGOMERY_255 1

/* ── SSL/TLS ── */
#define MBEDTLS_SSL_CLI_C              /* Client (https outbound) */
#define MBEDTLS_SSL_SRV_C              /* Server (tls.listen) */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_PROTO_TLS1_3      /* TLS 1.3 */
#define MBEDTLS_SSL_PROTO_TLS1_2      /* TLS 1.2 fallback */
#define MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL_ENABLED
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE
#define MBEDTLS_SSL_SERVER_NAME_INDICATION   /* SNI — required by virtual-host CDNs */
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED

/* ── Buffer sizes ── */
#define MBEDTLS_SSL_MAX_CONTENT_LEN    16384
#define MBEDTLS_SSL_IN_CONTENT_LEN     16384
#define MBEDTLS_SSL_OUT_CONTENT_LEN    4096
#define MBEDTLS_MPI_MAX_SIZE           512

#endif /* MBEDTLS_CONFIG_H */
