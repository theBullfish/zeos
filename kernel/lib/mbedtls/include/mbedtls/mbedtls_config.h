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

#define MBEDTLS_CONFIG_VERSION 0x04000000

/* ── Platform: bare-metal ── */
#define MBEDTLS_NO_PLATFORM_ENTROPY    /* We provide our own (TSC) */
#define MBEDTLS_NO_DEFAULT_ENTROPY_SOURCES
#define MBEDTLS_ENTROPY_HARDWARE_ALT   /* We implement the HW entropy hook */
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY        /* Custom malloc/free */
#define MBEDTLS_PLATFORM_NO_STD_FUNCTIONS

/* ── Crypto primitives ── */
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C                  /* AES-GCM (TLS 1.3 cipher) */
#define MBEDTLS_CCM_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_MD_C
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

/* ── Public key infrastructure ── */
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_X509_CRT_PARSE_C      /* Certificate parsing */
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRL_PARSE_C

/* ── RNG ── */
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_ENTROPY_C

/* ── SSL/TLS ── */
#define MBEDTLS_SSL_CLI_C              /* Client only — no server */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_PROTO_TLS1_3      /* TLS 1.3 */
#define MBEDTLS_SSL_PROTO_TLS1_2      /* TLS 1.2 fallback */
#define MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL_ENABLED

/* ── Buffer sizes ── */
#define MBEDTLS_SSL_MAX_CONTENT_LEN    16384
#define MBEDTLS_SSL_IN_CONTENT_LEN     16384
#define MBEDTLS_SSL_OUT_CONTENT_LEN    4096
#define MBEDTLS_MPI_MAX_SIZE           512

#endif /* MBEDTLS_CONFIG_H */
