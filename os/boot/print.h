/*
 * Zeos -- CHAIN_PRINT: Zeos-shaped print pipeline.
 *
 * Pipeline: print_request -> format -> tx_to_printer -> emit_completion
 *
 * Transports:
 *   - USB           (Printer class 0x07, bulk OUT for data)
 *   - IPP_NETWORK   (HTTP POST application/ipp to printer:631/ipp/print)
 *   - FILE_PDF      (write PDF representation to a fat32 path)
 *
 * Discovery: mDNS query for _ipp._tcp.local on UDP 5353 multicast
 * (224.0.0.251). Discovered printers auto-register.
 *
 * Document formats:
 *   - PDF passthrough
 *   - text -> auto-wrapped into a one-page PDF
 *   - PNG/JPEG -> single-page PDF (image stream)
 *
 * Subscriber chain CHAIN_PRINT_QUEUE tracks job state across resolves
 * and persists to VAULT so jobs survive reboot.
 *
 * NOT a daemon. Print is a typed signal stream; printers are subscribers;
 * the queue is one persistent consumer.
 */

#ifndef ZEOS_PRINT_H
#define ZEOS_PRINT_H

#include <stdint.h>
#include "net.h"

/* ── Limits ──────────────────────────────────────────────────────── */

#define PRINT_MAX_PRINTERS     8
#define PRINT_MAX_JOBS        16
#define PRINT_NAME_MAX        48
#define PRINT_ADDR_MAX        96
#define PRINT_DOC_MAX     262144   /* 256 KiB document buffer */
#define PRINT_PDF_OUT_MAX 524288   /* 512 KiB max generated PDF */
#define PRINT_JOB_ID_INVALID (0u)

/* ── Transports ──────────────────────────────────────────────────── */

typedef enum {
    PRINT_TRANSPORT_NONE        = 0,
    PRINT_TRANSPORT_USB         = 1,
    PRINT_TRANSPORT_IPP_NETWORK = 2,
    PRINT_TRANSPORT_FILE_PDF    = 3,
} print_transport_t;

/* ── Document formats ────────────────────────────────────────────── */

typedef enum {
    PRINT_FMT_AUTO = 0,
    PRINT_FMT_PDF  = 1,
    PRINT_FMT_TEXT = 2,
    PRINT_FMT_PNG  = 3,
    PRINT_FMT_JPEG = 4,
} print_format_t;

/* ── Capabilities ────────────────────────────────────────────────── */

typedef enum {
    PRINT_PAPER_LETTER = 0,
    PRINT_PAPER_A4     = 1,
    PRINT_PAPER_LEGAL  = 2,
} print_paper_t;

typedef struct {
    uint8_t  color;            /* 1 = color capable */
    uint8_t  duplex;           /* 1 = duplex capable */
    uint8_t  paper_mask;       /* bit0 LETTER, bit1 A4, bit2 LEGAL */
    uint16_t max_resolution;   /* DPI */
} print_caps_t;

/* ── Printer registry entry ──────────────────────────────────────── */

typedef enum {
    PRINTER_STATUS_IDLE    = 0,
    PRINTER_STATUS_BUSY    = 1,
    PRINTER_STATUS_ERROR   = 2,
    PRINTER_STATUS_OFFLINE = 3,
} printer_status_t;

typedef struct {
    uint8_t           in_use;
    uint8_t           id;
    uint8_t           transport;     /* print_transport_t */
    uint8_t           status;        /* printer_status_t */
    char              name[PRINT_NAME_MAX];

    /* Transport-specific addressing (a single union-like blob).
     *   USB:         vendor_id|product_id are populated.
     *   IPP_NETWORK: ip + port; addr holds "host:port/path".
     *   FILE_PDF:    addr holds a fat32 path. */
    char              addr[PRINT_ADDR_MAX];
    uint16_t          vendor_id;
    uint16_t          product_id;
    struct ipv4_addr  ip;
    uint16_t          port;

    print_caps_t      caps;

    /* Stats */
    uint32_t          jobs_total;
    uint32_t          jobs_failed;
    uint64_t          last_used_tsc;
} printer_info_t;

/* ── Job state ───────────────────────────────────────────────────── */

typedef enum {
    PRINT_JOB_PENDING   = 0,
    PRINT_JOB_PRINTING  = 1,
    PRINT_JOB_DONE      = 2,
    PRINT_JOB_FAILED    = 3,
    PRINT_JOB_CANCELLED = 4,
} print_job_state_t;

/* ── Print request (input to CHAIN_PRINT) ────────────────────────── */

typedef struct {
    uint8_t          valid;
    uint8_t          format;         /* print_format_t */
    uint8_t          target_printer_id;
    uint8_t          copies;
    uint8_t          paper_size;     /* print_paper_t */
    uint8_t          duplex;
    uint16_t         _pad;
    uint32_t         job_id;         /* assigned by submitter */
    uint32_t         len;
    const uint8_t   *bytes;          /* document bytes, NOT owned */
    char             title[PRINT_NAME_MAX];
} print_request_t;

/* ── Print completion (output of CHAIN_PRINT) ────────────────────── */

typedef struct {
    uint32_t  job_id;
    uint8_t   status;          /* print_job_state_t */
    uint8_t   target_printer_id;
    uint16_t  pages_printed;
    int32_t   error_code;      /* 0 ok; <0 = transport error */
    char      error_text[40];
} print_completion_t;

/* ── Job record (persistent, queued in CHAIN_PRINT_QUEUE) ────────── */

typedef struct {
    uint32_t job_id;
    uint8_t  state;             /* print_job_state_t */
    uint8_t  printer_id;
    uint8_t  format;
    uint8_t  copies;
    uint16_t pages_printed;
    uint16_t _pad;
    uint32_t len;
    int32_t  error_code;
    uint64_t submitted_tsc;
    uint64_t completed_tsc;
    char     title[PRINT_NAME_MAX];
    char     error_text[40];
} print_job_t;

/* ── Public chain ids (-1 until registered) ──────────────────────── */

extern int CHAIN_PRINT;
extern int CHAIN_PRINT_QUEUE;

/* ── Lifecycle ───────────────────────────────────────────────────── */

void print_init(void);
int  print_chain_register(int parent_chain_id);

/* ── Submission ──────────────────────────────────────────────────── */

/* Submit a job. Copies the document bytes into module-private buffer
 * and stages a request through CHAIN_PRINT. Returns job_id (>0) on
 * success, 0 on failure. */
uint32_t print_submit(const char *title,
                      print_format_t fmt,
                      const void *bytes, uint32_t len,
                      int target_printer_id, uint8_t copies);

/* Cancel a queued or in-flight job (best effort). Returns 0 on success. */
int print_cancel(uint32_t job_id);

/* ── Printer registry ────────────────────────────────────────────── */

int print_printer_count(void);
int print_printer_get(int idx, printer_info_t *out);
int print_printer_add_ipp(const char *name, const char *url);
int print_printer_add_file_pdf(const char *name, const char *path);
int print_printer_remove(int id);

/* ── Job queue ───────────────────────────────────────────────────── */

int print_job_count(void);
int print_job_get(int idx, print_job_t *out);
int print_job_active_count(void);

/* ── Discovery ───────────────────────────────────────────────────── */

/* Synchronous mDNS query for _ipp._tcp.local. Returns the number of
 * printers added. Best-effort — returns 0 if no NIC up or no replies. */
int print_mdns_discover(void);

/* USB printer scan: walks xHCI device list, finds class 0x07 interfaces,
 * registers each. Returns the number added. */
int print_usb_scan(void);

/* ── PDF generation (visible for testing) ────────────────────────── */

/* Write a minimal one-page PDF representation of `text` (ASCII) to
 * `out` (capacity `cap`). Returns bytes written, or -1. */
int print_pdf_from_text(const char *text, uint32_t text_len,
                        uint8_t *out, uint32_t cap);

/* Wrap a PNG/JPEG image as a single-page PDF (image stream). */
int print_pdf_from_image(const uint8_t *img, uint32_t img_len,
                         print_format_t fmt,
                         uint32_t pixel_w, uint32_t pixel_h,
                         uint8_t *out, uint32_t cap);

/* ── Stats ───────────────────────────────────────────────────────── */

uint32_t print_chain_jobs_total(void);
uint32_t print_chain_jobs_active(void);

/* ── Shell + selftest ────────────────────────────────────────────── */

void print_cmd(const char *args);
void printers_cmd(const char *args);
void printer_cmd(const char *args);
void print_queue_cmd(const char *args);
void print_cancel_cmd(const char *args);
void print_print_selftest_line(void);

#endif /* ZEOS_PRINT_H */
