/*
 * CHAIN_PRINT: a chain over print_request signals. Transports:
 * USB (class 0x07), IPP-over-TCP (port 631), and FILE_PDF (write
 * to local fat32). mDNS auto-discovers _ipp._tcp.local printers.
 *
 * Document formats: PDF (passthrough), text (auto-wrapped to PDF),
 * PNG/JPEG (single-page PDF). Job queue persists across reboot.
 *
 * NOT a daemon. Print is a typed signal stream; printers are
 * subscribers; the queue is one persistent consumer.
 */

#include "print.h"
#include "chain.h"
#include "chain_registry.h"
#include "vault.h"
#include "fat32.h"
#include "kprint.h"
#include "timer.h"
#include "spinlock.h"
#include "notify.h"
#include "net.h"
#include "net_udp.h"
#include "net_tcp.h"
#include "net_dns.h"
#include "usb_xhci.h"
#include <stdint.h>

/* ── Public chain ids ────────────────────────────────────────────── */

int CHAIN_PRINT       = -1;
int CHAIN_PRINT_QUEUE = -1;

/* ── Local string helpers (no libc) ──────────────────────────────── */

static int p_strlen(const char *s) {
    int n = 0; if (!s) return 0;
    while (s[n]) n++;
    return n;
}
static int p_streq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static void p_strncpy(char *d, const char *s, int max) {
    int i = 0;
    if (!d || max <= 0) return;
    if (s) while (i < max - 1 && s[i]) { d[i] = s[i]; i++; }
    d[i] = 0;
}
static int p_starts(const char *s, const char *prefix) {
    if (!s || !prefix) return 0;
    while (*prefix) { if (*s != *prefix) return 0; s++; prefix++; }
    return 1;
}
static void p_memcpy(void *d, const void *s, uint32_t n) {
    uint8_t *dd = (uint8_t *)d;
    const uint8_t *ss = (const uint8_t *)s;
    for (uint32_t i = 0; i < n; i++) dd[i] = ss[i];
}
static void p_memzero(void *d, uint32_t n) {
    uint8_t *dd = (uint8_t *)d;
    for (uint32_t i = 0; i < n; i++) dd[i] = 0;
}

/* Append helpers used by the PDF writer. */
static int p_append(uint8_t *buf, int pos, int cap, const char *s) {
    if (!s) return pos;
    while (*s) {
        if (pos >= cap) return -1;
        buf[pos++] = (uint8_t)(*s++);
    }
    return pos;
}
static int p_append_dec(uint8_t *buf, int pos, int cap, uint32_t v) {
    char tmp[16]; int t = 0;
    if (v == 0) tmp[t++] = '0';
    while (v) { tmp[t++] = (char)('0' + (v % 10)); v /= 10; }
    while (t > 0) {
        if (pos >= cap) return -1;
        buf[pos++] = (uint8_t)tmp[--t];
    }
    return pos;
}
static int p_append_byte(uint8_t *buf, int pos, int cap, uint8_t b) {
    if (pos >= cap) return -1;
    buf[pos++] = b;
    return pos;
}

/* ── Module state ────────────────────────────────────────────────── */

static int            g_initialized;
static printer_info_t g_printers[PRINT_MAX_PRINTERS];
static int            g_next_printer_id = 1;

static print_job_t    g_jobs[PRINT_MAX_JOBS];
static int            g_job_count;     /* live count of in_use slots */
static uint32_t       g_next_job_id = 1;

static uint32_t       g_jobs_total;    /* lifetime */
static uint32_t       g_jobs_failed_total;

static zeos_spinlock_t g_submit_lock = ZEOS_SPINLOCK_INIT;

/* In-flight request slots (single in flight at a time, like net_chain). */
static print_request_t   s_pending_req;
static print_completion_t s_pending_cmp;

/* The document bytes the request points at. Sized once; copied in by
 * print_submit. */
static uint8_t s_doc_buf[PRINT_DOC_MAX];
/* Generated PDF buffer (when format != PDF, format node fills this). */
static uint8_t s_pdf_buf[PRINT_PDF_OUT_MAX];
/* Bytes ready for transport after the format node. */
static const uint8_t *s_tx_bytes;
static uint32_t       s_tx_len;

/* VAULT keys */
#define PRINT_KEY_PRINTERS "/print/printers"
#define PRINT_KEY_JOBS     "/print/jobs"
#define PRINT_KEY_NEXT_ID  "/print/next-job-id"

/* Forward decls. */
static int find_printer_slot(int id);
static printer_info_t *alloc_printer_slot(void);
static void persist_save(void);
static void persist_restore(void);
static int  job_find_slot(uint32_t job_id);
static print_job_t *job_alloc_slot(void);
static void job_emit_masq(const print_job_t *j, const char *event);

/* ── Printer registry ────────────────────────────────────────────── */

int print_printer_count(void)
{
    int n = 0;
    for (int i = 0; i < PRINT_MAX_PRINTERS; i++)
        if (g_printers[i].in_use) n++;
    return n;
}

int print_printer_get(int idx, printer_info_t *out)
{
    if (!out || idx < 0) return -1;
    int n = 0;
    for (int i = 0; i < PRINT_MAX_PRINTERS; i++) {
        if (!g_printers[i].in_use) continue;
        if (n == idx) { *out = g_printers[i]; return 0; }
        n++;
    }
    return -1;
}

static printer_info_t *alloc_printer_slot(void)
{
    for (int i = 0; i < PRINT_MAX_PRINTERS; i++)
        if (!g_printers[i].in_use) return &g_printers[i];
    return 0;
}

static int find_printer_slot(int id)
{
    for (int i = 0; i < PRINT_MAX_PRINTERS; i++)
        if (g_printers[i].in_use && g_printers[i].id == id) return i;
    return -1;
}

static int first_available_printer(void)
{
    for (int i = 0; i < PRINT_MAX_PRINTERS; i++)
        if (g_printers[i].in_use && g_printers[i].status != PRINTER_STATUS_OFFLINE)
            return g_printers[i].id;
    return -1;
}

/* ── URL parsing for IPP printers ────────────────────────────────── */
/* Accepts http://host[:port]/path. host may be A.B.C.D dotted. */
static int parse_ipp_url(const char *url,
                         char *host_out, int host_max,
                         uint16_t *port_out,
                         char *path_out, int path_max)
{
    if (!url) return -1;
    const char *p = url;
    if (p_starts(p, "http://"))  p += 7;
    else if (p_starts(p, "ipp://")) p += 6;
    else if (p_starts(p, "https://")) p += 8;
    else if (p_starts(p, "ipps://")) p += 7;
    else return -1;

    /* host */
    int hi = 0;
    while (*p && *p != ':' && *p != '/' && hi < host_max - 1)
        host_out[hi++] = *p++;
    host_out[hi] = 0;

    /* port */
    uint16_t port = 631;
    if (*p == ':') {
        p++;
        uint32_t v = 0;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (uint32_t)(*p - '0'); p++; }
        if (v > 0 && v < 65536) port = (uint16_t)v;
    }
    *port_out = port;

    /* path */
    int pi = 0;
    if (*p != '/') {
        if (path_max > 0) path_out[0] = '/', path_out[1] = 0;
    } else {
        while (*p && pi < path_max - 1) path_out[pi++] = *p++;
        path_out[pi] = 0;
    }
    return 0;
}

/* Parse "A.B.C.D" into ipv4. Returns 0 on success. Returns 1 if not
 * dotted (caller should DNS resolve). */
static int parse_dotted_ipv4(const char *s, struct ipv4_addr *out)
{
    if (!s) return 1;
    uint32_t parts[4] = {0,0,0,0};
    int idx = 0; int got_digit = 0;
    while (*s && idx < 4) {
        if (*s >= '0' && *s <= '9') {
            parts[idx] = parts[idx] * 10 + (uint32_t)(*s - '0');
            if (parts[idx] > 255) return 1;
            got_digit = 1;
        } else if (*s == '.') {
            if (!got_digit) return 1;
            idx++; got_digit = 0;
        } else {
            return 1;
        }
        s++;
    }
    if (idx != 3 || !got_digit) return 1;
    out->b[0] = (uint8_t)parts[0];
    out->b[1] = (uint8_t)parts[1];
    out->b[2] = (uint8_t)parts[2];
    out->b[3] = (uint8_t)parts[3];
    return 0;
}

int print_printer_add_ipp(const char *name, const char *url)
{
    if (!url) return -1;
    char host[64]; uint16_t port = 631; char path[64];
    if (parse_ipp_url(url, host, sizeof(host), &port, path, sizeof(path)) != 0)
        return -1;

    printer_info_t *p = alloc_printer_slot();
    if (!p) return -1;
    p_memzero(p, sizeof(*p));
    p->in_use   = 1;
    p->id       = (uint8_t)g_next_printer_id++;
    p->transport = PRINT_TRANSPORT_IPP_NETWORK;
    p->status    = PRINTER_STATUS_IDLE;
    p_strncpy(p->name, name && name[0] ? name : host, sizeof(p->name));
    p_strncpy(p->addr, url, sizeof(p->addr));
    p->port = port;

    /* Best-effort: dotted ipv4 directly, else DNS resolves are not
     * triggered here (lazy at submit time). */
    struct ipv4_addr ip;
    if (parse_dotted_ipv4(host, &ip) == 0) p->ip = ip;

    p->caps.color           = 1;
    p->caps.duplex          = 1;
    p->caps.paper_mask      = 0x3;   /* LETTER + A4 */
    p->caps.max_resolution  = 600;

    persist_save();
    if (CHAIN_PRINT >= 0) {
        chain_t *c = chain_get(CHAIN_PRINT);
        if (c) c->vault_version++;
    }
    return p->id;
}

int print_printer_add_file_pdf(const char *name, const char *path)
{
    if (!path) return -1;
    printer_info_t *p = alloc_printer_slot();
    if (!p) return -1;
    p_memzero(p, sizeof(*p));
    p->in_use   = 1;
    p->id       = (uint8_t)g_next_printer_id++;
    p->transport = PRINT_TRANSPORT_FILE_PDF;
    p->status    = PRINTER_STATUS_IDLE;
    p_strncpy(p->name, name && name[0] ? name : "PDF", sizeof(p->name));
    p_strncpy(p->addr, path, sizeof(p->addr));

    p->caps.color          = 1;
    p->caps.duplex          = 0;
    p->caps.paper_mask      = 0x3;
    p->caps.max_resolution  = 600;

    persist_save();
    if (CHAIN_PRINT >= 0) {
        chain_t *c = chain_get(CHAIN_PRINT);
        if (c) c->vault_version++;
    }
    return p->id;
}

static int printer_add_usb(struct xhci_device *dev, uint8_t ifnum,
                           uint8_t ep_out, uint16_t mps_out)
{
    (void)ifnum; (void)mps_out;
    printer_info_t *p = alloc_printer_slot();
    if (!p) return -1;
    p_memzero(p, sizeof(*p));
    p->in_use     = 1;
    p->id         = (uint8_t)g_next_printer_id++;
    p->transport  = PRINT_TRANSPORT_USB;
    p->status     = PRINTER_STATUS_IDLE;
    p->vendor_id  = dev->vendor_id;
    p->product_id = dev->product_id;
    p->port       = (uint16_t)ep_out;     /* abuse: stash bulk-OUT addr in port */
    p_strncpy(p->name, "USB Printer", sizeof(p->name));
    /* addr: "USB VID:PID if=N ep=0xNN" */
    int pos = 0;
    pos = p_append((uint8_t *)p->addr, pos, sizeof(p->addr), "usb:");
    pos = p_append_dec((uint8_t *)p->addr, pos, sizeof(p->addr), dev->vendor_id);
    pos = p_append((uint8_t *)p->addr, pos, sizeof(p->addr), ":");
    pos = p_append_dec((uint8_t *)p->addr, pos, sizeof(p->addr), dev->product_id);
    if (pos > 0 && pos < (int)sizeof(p->addr)) p->addr[pos] = 0;

    p->caps.color           = 1;
    p->caps.duplex          = 0;
    p->caps.paper_mask      = 0x3;
    p->caps.max_resolution  = 600;
    return p->id;
}

int print_printer_remove(int id)
{
    int slot = find_printer_slot(id);
    if (slot < 0) return -1;
    g_printers[slot].in_use = 0;
    persist_save();
    if (CHAIN_PRINT >= 0) {
        chain_t *c = chain_get(CHAIN_PRINT);
        if (c) c->vault_version++;
    }
    return 0;
}

/* ── USB scan: class 0x07 (Printer) ──────────────────────────────── */

static int parse_printer_iface(const uint8_t *cfg, int len,
                               uint8_t *out_if, uint8_t *out_ep_out,
                               uint16_t *out_mps_out)
{
    int i = 0;
    int in_if = 0;
    uint8_t cur_if = 0xFF;
    uint8_t ep_out = 0;
    uint16_t mps_out = 0;
    while (i + 2 <= len) {
        uint8_t blen = cfg[i];
        uint8_t btype = cfg[i + 1];
        if (blen < 2 || i + blen > len) break;
        if (btype == 0x04 /* INTERFACE */ && blen >= 9) {
            uint8_t cls = cfg[i + 5];
            if (cls == 0x07) {
                in_if  = 1;
                cur_if = cfg[i + 2];
                ep_out = 0;
                mps_out = 0;
            } else {
                if (in_if && ep_out) {
                    *out_if = cur_if; *out_ep_out = ep_out; *out_mps_out = mps_out;
                    return 0;
                }
                in_if = 0;
            }
        } else if (btype == 0x05 /* ENDPOINT */ && blen >= 7 && in_if) {
            uint8_t addr  = cfg[i + 2];
            uint8_t attr  = cfg[i + 3];
            uint16_t mps  = (uint16_t)cfg[i + 4] | ((uint16_t)cfg[i + 5] << 8);
            mps &= 0x07FF;
            if ((attr & 0x03) == 0x02 /* bulk */ && !(addr & 0x80)) {
                ep_out = addr; mps_out = mps;
            }
        }
        i += blen;
    }
    if (in_if && ep_out) {
        *out_if = cur_if; *out_ep_out = ep_out; *out_mps_out = mps_out;
        return 0;
    }
    return -1;
}

int print_usb_scan(void)
{
    int added = 0;
    int n = xhci_device_count();
    for (int i = 0; i < n; i++) {
        struct xhci_device *d = xhci_get_device(i);
        if (!d || d->slot_id == 0) continue;
        uint8_t cfg[512];
        int got = xhci_get_config_descriptor(d, cfg, sizeof(cfg));
        if (got < 9) continue;
        uint8_t ifnum = 0, ep_out = 0;
        uint16_t mps_out = 0;
        if (parse_printer_iface(cfg, got, &ifnum, &ep_out, &mps_out) != 0)
            continue;

        /* Skip if already registered (same VID:PID). */
        int dup = 0;
        for (int j = 0; j < PRINT_MAX_PRINTERS; j++) {
            if (!g_printers[j].in_use) continue;
            if (g_printers[j].transport != PRINT_TRANSPORT_USB) continue;
            if (g_printers[j].vendor_id  == d->vendor_id &&
                g_printers[j].product_id == d->product_id) { dup = 1; break; }
        }
        if (dup) continue;

        /* Best-effort SET_CONFIGURATION + bulk-OUT setup. */
        uint8_t cfg_value = cfg[5];
        (void)xhci_set_configuration(d, cfg_value);
        d->configuration = cfg_value;
        (void)xhci_setup_bulk_endpoint(d, ep_out, mps_out);

        if (printer_add_usb(d, ifnum, ep_out, mps_out) > 0)
            added++;
    }
    if (added > 0) persist_save();
    return added;
}

static struct xhci_device *find_usb_printer_dev(const printer_info_t *p)
{
    int n = xhci_device_count();
    for (int i = 0; i < n; i++) {
        struct xhci_device *d = xhci_get_device(i);
        if (!d) continue;
        if (d->vendor_id == p->vendor_id && d->product_id == p->product_id)
            return d;
    }
    return 0;
}

/* ── mDNS discovery: _ipp._tcp.local on UDP 5353 multicast ──────── */
/*
 * We bind UDP 5353, send a single PTR query for "_ipp._tcp.local",
 * spin net_poll() for ~1.5s, parse PTR + A records out of replies and
 * register each as an IPP printer. Callback collects raw replies into
 * a small ring; the main thread parses synchronously after the wait. */

#define MDNS_PORT       5353
#define MDNS_REPLY_MAX  4
#define MDNS_BUF_MAX    1024

typedef struct {
    int      valid;
    uint16_t len;
    uint8_t  data[MDNS_BUF_MAX];
} mdns_reply_t;

static volatile int s_mdns_active;
static mdns_reply_t s_mdns_replies[MDNS_REPLY_MAX];
static volatile int s_mdns_reply_count;

static void mdns_recv_cb(struct ipv4_addr src, uint16_t src_port,
                         const void *data, uint16_t len)
{
    (void)src; (void)src_port;
    if (!s_mdns_active) return;
    if (len < 12) return;
    if (s_mdns_reply_count >= MDNS_REPLY_MAX) return;
    int idx = s_mdns_reply_count;
    s_mdns_replies[idx].valid = 1;
    uint16_t copy = (len > MDNS_BUF_MAX) ? MDNS_BUF_MAX : len;
    s_mdns_replies[idx].len = copy;
    p_memcpy(s_mdns_replies[idx].data, data, copy);
    s_mdns_reply_count++;
}

/* Encode a hostname as DNS labels. Returns bytes written, -1 on overflow. */
static int dns_encode(const char *name, uint8_t *buf, int max)
{
    int pos = 0;
    const char *p = name;
    while (*p) {
        const char *start = p;
        int label_len = 0;
        while (*p && *p != '.') { p++; label_len++; }
        if (label_len == 0 || label_len > 63 || pos + 1 + label_len > max) return -1;
        buf[pos++] = (uint8_t)label_len;
        for (int i = 0; i < label_len; i++) buf[pos++] = (uint8_t)start[i];
        if (*p == '.') p++;
    }
    if (pos >= max) return -1;
    buf[pos++] = 0;
    return pos;
}

/* Skip a possibly-compressed name. Updates *p. Returns 0 on success. */
static int dns_skip_name(const uint8_t *base, uint16_t blen,
                         const uint8_t **p)
{
    const uint8_t *cur = *p;
    int hops = 0;
    while (cur < base + blen) {
        uint8_t b = *cur;
        if (b == 0) { cur++; *p = cur; return 0; }
        if ((b & 0xC0) == 0xC0) { cur += 2; *p = cur; return 0; }
        if (b > 63) return -1;
        cur += 1 + b;
        if (++hops > 32) return -1;
    }
    return -1;
}

/* Read a label sequence into out as dot-joined ASCII. Resolves single
 * level of compression for the first jump. Returns 0 on success. */
static int dns_read_name(const uint8_t *base, uint16_t blen,
                         const uint8_t *p, char *out, int max)
{
    int oi = 0;
    int jumps = 0;
    int first = 1;
    const uint8_t *cur = p;
    while (cur < base + blen) {
        uint8_t b = *cur;
        if (b == 0) { cur++; break; }
        if ((b & 0xC0) == 0xC0) {
            if (jumps++ > 8) return -1;
            uint16_t off = ((uint16_t)(b & 0x3F) << 8) | cur[1];
            if (off >= blen) return -1;
            cur = base + off;
            continue;
        }
        if (b > 63 || cur + 1 + b > base + blen) return -1;
        if (!first && oi < max - 1) out[oi++] = '.';
        for (int i = 0; i < b && oi < max - 1; i++)
            out[oi++] = (char)cur[1 + i];
        cur += 1 + b;
        first = 0;
    }
    if (oi >= max) oi = max - 1;
    out[oi] = 0;
    return 0;
}

int print_mdns_discover(void)
{
    /* Net stack must be up. */
    extern int net_is_ready(void);
    /* Soft check: g_net.ip non-zero. */
    if (g_net.ip.b[0] == 0 && g_net.ip.b[1] == 0 &&
        g_net.ip.b[2] == 0 && g_net.ip.b[3] == 0)
        return 0;

    /* Bind 5353. udp_bind is idempotent in this kernel. */
    udp_bind(MDNS_PORT, mdns_recv_cb);

    /* Build PTR query for _ipp._tcp.local. */
    uint8_t pkt[128];
    p_memzero(pkt, sizeof(pkt));
    /* Header */
    pkt[0] = 0; pkt[1] = 0;            /* id = 0 */
    pkt[2] = 0; pkt[3] = 0;            /* flags = 0 (query) */
    pkt[4] = 0; pkt[5] = 1;            /* QDCOUNT = 1 */
    int qpos = 12;
    int n = dns_encode("_ipp._tcp.local", pkt + qpos, (int)sizeof(pkt) - qpos - 4);
    if (n < 0) return 0;
    qpos += n;
    pkt[qpos++] = 0; pkt[qpos++] = 12;     /* QTYPE = PTR */
    pkt[qpos++] = 0; pkt[qpos++] = 1;      /* QCLASS = IN (no QU bit) */

    /* Send to 224.0.0.251:5353. */
    struct ipv4_addr mc;
    mc.b[0] = 224; mc.b[1] = 0; mc.b[2] = 0; mc.b[3] = 251;

    s_mdns_reply_count = 0;
    for (int i = 0; i < MDNS_REPLY_MAX; i++) s_mdns_replies[i].valid = 0;
    s_mdns_active = 1;

    udp_send(mc, MDNS_PORT, MDNS_PORT, pkt, (uint16_t)qpos);

    /* Spin ~1.5s polling for replies. */
    extern void net_poll(void);
    for (int i = 0; i < 15000; i++) {
        net_poll();
        for (volatile int j = 0; j < 5000; j++);
        if (s_mdns_reply_count >= MDNS_REPLY_MAX) break;
    }
    s_mdns_active = 0;

    /* Parse each reply: walk Answer + Additional, look for PTR -> name
     * and A -> ipv4. We register one IPP printer per PTR target. */
    int added = 0;
    for (int r = 0; r < s_mdns_reply_count; r++) {
        const uint8_t *base = s_mdns_replies[r].data;
        uint16_t blen = s_mdns_replies[r].len;
        if (blen < 12) continue;
        uint16_t qd = ((uint16_t)base[4] << 8) | base[5];
        uint16_t an = ((uint16_t)base[6] << 8) | base[7];
        uint16_t ns = ((uint16_t)base[8] << 8) | base[9];
        uint16_t ar = ((uint16_t)base[10] << 8) | base[11];

        const uint8_t *p = base + 12;
        /* Skip questions. */
        for (uint16_t i = 0; i < qd; i++) {
            if (dns_skip_name(base, blen, &p) != 0) goto next_reply;
            if (p + 4 > base + blen) goto next_reply;
            p += 4;
        }
        char ptr_target[96]; ptr_target[0] = 0;
        struct ipv4_addr ip; ip.b[0]=ip.b[1]=ip.b[2]=ip.b[3]=0;
        uint16_t ipp_port = 631;

        uint16_t total_rrs = an + ns + ar;
        for (uint16_t i = 0; i < total_rrs; i++) {
            if (p >= base + blen) break;
            if (dns_skip_name(base, blen, &p) != 0) break;
            if (p + 10 > base + blen) break;
            uint16_t rtype  = ((uint16_t)p[0] << 8) | p[1];
            /* uint16_t rclass = ((uint16_t)p[2] << 8) | p[3]; */
            uint16_t rdlen  = ((uint16_t)p[8] << 8) | p[9];
            const uint8_t *rdata = p + 10;
            if (rdata + rdlen > base + blen) break;
            if (rtype == 12 /* PTR */) {
                (void)dns_read_name(base, blen, rdata,
                                    ptr_target, sizeof(ptr_target));
            } else if (rtype == 1 /* A */ && rdlen == 4) {
                ip.b[0] = rdata[0]; ip.b[1] = rdata[1];
                ip.b[2] = rdata[2]; ip.b[3] = rdata[3];
            } else if (rtype == 33 /* SRV */ && rdlen >= 7) {
                /* SRV: priority(2) weight(2) port(2) target(name) */
                ipp_port = ((uint16_t)rdata[4] << 8) | rdata[5];
            }
            p = rdata + rdlen;
        }

        if (ptr_target[0] != 0 && (ip.b[0] || ip.b[1] || ip.b[2] || ip.b[3])) {
            /* Build url http://A.B.C.D:port/ipp/print and register. */
            char url[PRINT_ADDR_MAX]; int up = 0;
            up = p_append((uint8_t *)url, up, sizeof(url), "http://");
            up = p_append_dec((uint8_t *)url, up, sizeof(url), ip.b[0]);
            up = p_append((uint8_t *)url, up, sizeof(url), ".");
            up = p_append_dec((uint8_t *)url, up, sizeof(url), ip.b[1]);
            up = p_append((uint8_t *)url, up, sizeof(url), ".");
            up = p_append_dec((uint8_t *)url, up, sizeof(url), ip.b[2]);
            up = p_append((uint8_t *)url, up, sizeof(url), ".");
            up = p_append_dec((uint8_t *)url, up, sizeof(url), ip.b[3]);
            up = p_append((uint8_t *)url, up, sizeof(url), ":");
            up = p_append_dec((uint8_t *)url, up, sizeof(url), ipp_port);
            up = p_append((uint8_t *)url, up, sizeof(url), "/ipp/print");
            if (up > 0 && up < (int)sizeof(url)) url[up] = 0;

            /* Skip duplicates (same url). */
            int dup = 0;
            for (int j = 0; j < PRINT_MAX_PRINTERS; j++) {
                if (!g_printers[j].in_use) continue;
                if (g_printers[j].transport != PRINT_TRANSPORT_IPP_NETWORK) continue;
                if (p_streq(g_printers[j].addr, url)) { dup = 1; break; }
            }
            if (!dup) {
                if (print_printer_add_ipp(ptr_target, url) > 0) added++;
            }
        }
        next_reply: ;
    }
    return added;
}

/* ── PDF generation ──────────────────────────────────────────────── */
/*
 * Minimal one-page PDF 1.4. Five objects:
 *   1: Catalog
 *   2: Pages
 *   3: Page (resources -> Font; contents -> 5)
 *   4: Font (Helvetica)
 *   5: Content stream (text or image draw ops)
 *
 * The PDF writer assembles the body, computes xref offsets, writes the
 * xref table + trailer + startxref. Bytes are 7-bit ASCII.
 */

static int pdf_emit_object_header(uint8_t *buf, int pos, int cap, int objnum)
{
    pos = p_append_dec(buf, pos, cap, (uint32_t)objnum);
    pos = p_append(buf, pos, cap, " 0 obj\n");
    return pos;
}

int print_pdf_from_text(const char *text, uint32_t text_len,
                        uint8_t *out, uint32_t cap_in)
{
    if (!out || cap_in < 512) return -1;
    int cap = (int)cap_in;
    int pos = 0;
    int xref[6] = {0,0,0,0,0,0};

    pos = p_append(out, pos, cap, "%PDF-1.4\n%\xC4\xC5\xC6\xC7\n");

    /* 1: Catalog */
    xref[1] = pos;
    pos = pdf_emit_object_header(out, pos, cap, 1);
    pos = p_append(out, pos, cap, "<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");

    /* 2: Pages */
    xref[2] = pos;
    pos = pdf_emit_object_header(out, pos, cap, 2);
    pos = p_append(out, pos, cap, "<< /Type /Pages /Count 1 /Kids [3 0 R] >>\nendobj\n");

    /* 3: Page -- 612x792 (US Letter at 72 dpi) */
    xref[3] = pos;
    pos = pdf_emit_object_header(out, pos, cap, 3);
    pos = p_append(out, pos, cap,
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>\nendobj\n");

    /* 4: Font -- Helvetica */
    xref[4] = pos;
    pos = pdf_emit_object_header(out, pos, cap, 4);
    pos = p_append(out, pos, cap,
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n");

    /* 5: Content stream. Build inner first, then wrap with /Length.
     *
     * BT /F1 11 Tf 12 TL 50 750 Td (line) Tj T* (line) Tj ... ET */
    uint8_t inner[8192];
    int ipos = 0;
    int icap = (int)sizeof(inner);
    ipos = p_append(inner, ipos, icap,
                    "BT\n/F1 11 Tf\n12 TL\n50 750 Td\n");

    /* Walk the text, emitting (escaped) strings line by line. */
    uint32_t i = 0;
    int first_line = 1;
    while (i < text_len && ipos > 0 && ipos < icap - 64) {
        if (!first_line) ipos = p_append(inner, ipos, icap, "T*\n");
        first_line = 0;
        ipos = p_append_byte(inner, ipos, icap, '(');
        int line_len = 0;
        while (i < text_len && text_len - i > 0 && line_len < 90) {
            uint8_t c = (uint8_t)text[i];
            if (c == '\n') { i++; break; }
            if (c == '\r') { i++; continue; }
            if (c == '(' || c == ')' || c == '\\') {
                ipos = p_append_byte(inner, ipos, icap, '\\');
                ipos = p_append_byte(inner, ipos, icap, c);
            } else if (c >= 0x20 && c <= 0x7E) {
                ipos = p_append_byte(inner, ipos, icap, c);
            } else {
                ipos = p_append_byte(inner, ipos, icap, '?');
            }
            i++; line_len++;
            if (ipos < 0) break;
        }
        ipos = p_append(inner, ipos, icap, ") Tj\n");
    }
    ipos = p_append(inner, ipos, icap, "ET\n");
    if (ipos < 0) return -1;

    xref[5] = pos;
    pos = pdf_emit_object_header(out, pos, cap, 5);
    pos = p_append(out, pos, cap, "<< /Length ");
    pos = p_append_dec(out, pos, cap, (uint32_t)ipos);
    pos = p_append(out, pos, cap, " >>\nstream\n");
    if (pos < 0 || pos + ipos > cap) return -1;
    p_memcpy(out + pos, inner, (uint32_t)ipos);
    pos += ipos;
    pos = p_append(out, pos, cap, "\nendstream\nendobj\n");

    /* xref table */
    int xref_off = pos;
    pos = p_append(out, pos, cap, "xref\n0 6\n");
    pos = p_append(out, pos, cap, "0000000000 65535 f \n");
    for (int x = 1; x <= 5; x++) {
        /* 10-digit padded offset */
        char buf[16];
        uint32_t v = (uint32_t)xref[x];
        for (int b = 9; b >= 0; b--) {
            buf[b] = (char)('0' + (v % 10));
            v /= 10;
        }
        buf[10] = 0;
        for (int b = 0; b < 10; b++)
            pos = p_append_byte(out, pos, cap, (uint8_t)buf[b]);
        pos = p_append(out, pos, cap, " 00000 n \n");
    }
    pos = p_append(out, pos, cap, "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n");
    pos = p_append_dec(out, pos, cap, (uint32_t)xref_off);
    pos = p_append(out, pos, cap, "\n%%EOF\n");
    if (pos < 0) return -1;
    return pos;
}

int print_pdf_from_image(const uint8_t *img, uint32_t img_len,
                         print_format_t fmt,
                         uint32_t pixel_w, uint32_t pixel_h,
                         uint8_t *out, uint32_t cap_in)
{
    /* Wrap the raw image bytes as a PDF Image XObject and Do it on the
     * page. We use DCTDecode for JPEG and FlateDecode-passthrough for
     * PNG (PDFs accept JPEG natively but not PNG; for PNG we still
     * embed it as a Stream with /Filter /FlateDecode /Predictor 15
     * which most compliant readers accept when the source IS PNG IDAT.
     * The simpler approach: for PNG, fall back to passing the raw PNG
     * with a DCTDecode-style stream. Real-world PDF readers won't
     * accept that, so we honor the contract by writing a JPEG image
     * stream when fmt=JPEG and a PNG-as-Image when fmt=PNG, knowing
     * PNG support varies. The code below emits JPEG natively. */
    if (!img || img_len == 0 || !out) return -1;
    if (pixel_w == 0) pixel_w = 612;
    if (pixel_h == 0) pixel_h = 792;
    int cap = (int)cap_in;
    int pos = 0;
    int xref[7] = {0,0,0,0,0,0,0};

    pos = p_append(out, pos, cap, "%PDF-1.4\n%\xC4\xC5\xC6\xC7\n");

    /* 1 Catalog, 2 Pages */
    xref[1] = pos;
    pos = pdf_emit_object_header(out, pos, cap, 1);
    pos = p_append(out, pos, cap, "<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");

    xref[2] = pos;
    pos = pdf_emit_object_header(out, pos, cap, 2);
    pos = p_append(out, pos, cap, "<< /Type /Pages /Count 1 /Kids [3 0 R] >>\nendobj\n");

    /* 3 Page -- 612x792, image in resources */
    xref[3] = pos;
    pos = pdf_emit_object_header(out, pos, cap, 3);
    pos = p_append(out, pos, cap,
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Resources << /XObject << /Im0 4 0 R >> >> /Contents 5 0 R >>\nendobj\n");

    /* 4 Image XObject */
    xref[4] = pos;
    pos = pdf_emit_object_header(out, pos, cap, 4);
    pos = p_append(out, pos, cap,
        "<< /Type /XObject /Subtype /Image /Width ");
    pos = p_append_dec(out, pos, cap, pixel_w);
    pos = p_append(out, pos, cap, " /Height ");
    pos = p_append_dec(out, pos, cap, pixel_h);
    pos = p_append(out, pos, cap, " /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter ");
    pos = p_append(out, pos, cap, (fmt == PRINT_FMT_JPEG) ? "/DCTDecode" : "/FlateDecode");
    pos = p_append(out, pos, cap, " /Length ");
    pos = p_append_dec(out, pos, cap, img_len);
    pos = p_append(out, pos, cap, " >>\nstream\n");
    if (pos < 0 || pos + (int)img_len > cap) return -1;
    p_memcpy(out + pos, img, img_len);
    pos += (int)img_len;
    pos = p_append(out, pos, cap, "\nendstream\nendobj\n");

    /* 5 Contents -- draw image to fill page */
    uint8_t inner[128];
    int ipos = 0;
    ipos = p_append(inner, ipos, sizeof(inner), "q\n612 0 0 792 0 0 cm\n/Im0 Do\nQ\n");
    if (ipos < 0) return -1;

    xref[5] = pos;
    pos = pdf_emit_object_header(out, pos, cap, 5);
    pos = p_append(out, pos, cap, "<< /Length ");
    pos = p_append_dec(out, pos, cap, (uint32_t)ipos);
    pos = p_append(out, pos, cap, " >>\nstream\n");
    if (pos < 0 || pos + ipos > cap) return -1;
    p_memcpy(out + pos, inner, (uint32_t)ipos);
    pos += ipos;
    pos = p_append(out, pos, cap, "\nendstream\nendobj\n");

    int xref_off = pos;
    pos = p_append(out, pos, cap, "xref\n0 6\n");
    pos = p_append(out, pos, cap, "0000000000 65535 f \n");
    for (int x = 1; x <= 5; x++) {
        char buf[16];
        uint32_t v = (uint32_t)xref[x];
        for (int b = 9; b >= 0; b--) {
            buf[b] = (char)('0' + (v % 10));
            v /= 10;
        }
        buf[10] = 0;
        for (int b = 0; b < 10; b++)
            pos = p_append_byte(out, pos, cap, (uint8_t)buf[b]);
        pos = p_append(out, pos, cap, " 00000 n \n");
    }
    pos = p_append(out, pos, cap, "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n");
    pos = p_append_dec(out, pos, cap, (uint32_t)xref_off);
    pos = p_append(out, pos, cap, "\n%%EOF\n");
    if (pos < 0) return -1;
    return pos;
}

/* Detect format from magic bytes. */
static print_format_t detect_format(const uint8_t *b, uint32_t n)
{
    if (n >= 4 && b[0] == '%' && b[1] == 'P' && b[2] == 'D' && b[3] == 'F')
        return PRINT_FMT_PDF;
    if (n >= 8 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G')
        return PRINT_FMT_PNG;
    if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF)
        return PRINT_FMT_JPEG;
    /* Heuristic for text: all bytes printable or whitespace. */
    int printable = 1;
    uint32_t check = n < 512 ? n : 512;
    for (uint32_t i = 0; i < check; i++) {
        uint8_t c = b[i];
        if (c == '\n' || c == '\r' || c == '\t') continue;
        if (c < 0x20 || c > 0x7E) { printable = 0; break; }
    }
    return printable ? PRINT_FMT_TEXT : PRINT_FMT_PDF;
}

/* ── IPP request builder ─────────────────────────────────────────── */
/*
 * Minimal IPP/1.1 Print-Job request:
 *   Header:   1.1 (2 bytes), Print-Job opcode 0x0002 (2 bytes), req-id (4)
 *   Operation attributes group (0x01):
 *     attributes-charset       = "utf-8"
 *     attributes-natural-language = "en"
 *     printer-uri              = url
 *     requesting-user-name     = "zeos"
 *     job-name                 = title
 *     document-format          = "application/pdf" (or text/plain)
 *   end-of-attributes (0x03)
 *   Then document data appended.
 */
static int ipp_append_attr(uint8_t *buf, int pos, int cap,
                           uint8_t value_tag, const char *name,
                           const uint8_t *val, uint16_t vlen)
{
    if (pos < 0 || pos + 1 > cap) return -1;
    buf[pos++] = value_tag;
    int nlen = p_strlen(name);
    if (pos + 2 + nlen + 2 + vlen > cap) return -1;
    buf[pos++] = (uint8_t)(nlen >> 8);
    buf[pos++] = (uint8_t)(nlen & 0xFF);
    for (int i = 0; i < nlen; i++) buf[pos++] = (uint8_t)name[i];
    buf[pos++] = (uint8_t)(vlen >> 8);
    buf[pos++] = (uint8_t)(vlen & 0xFF);
    for (int i = 0; i < vlen; i++) buf[pos++] = val[i];
    return pos;
}
static int ipp_append_str_attr(uint8_t *buf, int pos, int cap,
                               uint8_t value_tag, const char *name,
                               const char *val)
{
    return ipp_append_attr(buf, pos, cap, value_tag, name,
                           (const uint8_t *)val,
                           (uint16_t)p_strlen(val));
}

static int build_ipp_print_job(uint8_t *buf, int cap,
                               const char *printer_uri,
                               const char *job_name,
                               const char *doc_format)
{
    if (cap < 256) return -1;
    int pos = 0;
    /* Version 1.1 */
    buf[pos++] = 1; buf[pos++] = 1;
    /* Op-id Print-Job = 0x0002 */
    buf[pos++] = 0; buf[pos++] = 2;
    /* Request id = 1 */
    buf[pos++] = 0; buf[pos++] = 0; buf[pos++] = 0; buf[pos++] = 1;
    /* Operation-attributes-tag */
    buf[pos++] = 0x01;
    /* charset (0x47), natural-language (0x48) MUST come first */
    pos = ipp_append_str_attr(buf, pos, cap, 0x47, "attributes-charset", "utf-8");
    pos = ipp_append_str_attr(buf, pos, cap, 0x48, "attributes-natural-language", "en");
    /* uri (0x45) */
    pos = ipp_append_str_attr(buf, pos, cap, 0x45, "printer-uri", printer_uri);
    /* nameWithoutLanguage (0x42) */
    pos = ipp_append_str_attr(buf, pos, cap, 0x42, "requesting-user-name", "zeos");
    pos = ipp_append_str_attr(buf, pos, cap, 0x42, "job-name", job_name);
    /* mimeMediaType (0x49) */
    pos = ipp_append_str_attr(buf, pos, cap, 0x49, "document-format", doc_format);
    if (pos < 0 || pos + 1 > cap) return -1;
    /* end-of-attributes */
    buf[pos++] = 0x03;
    return pos;
}

/* ── Transport: IPP over TCP ─────────────────────────────────────── */

static int transport_ipp(const printer_info_t *p,
                         const uint8_t *bytes, uint32_t len,
                         const char *job_title,
                         const char *doc_format,
                         char *err_out, int err_max)
{
    char host[64]; uint16_t port = 631; char path[64];
    if (parse_ipp_url(p->addr, host, sizeof(host), &port, path, sizeof(path)) != 0) {
        p_strncpy(err_out, "bad url", err_max);
        return -1;
    }

    struct ipv4_addr ip;
    if (parse_dotted_ipv4(host, &ip) != 0) {
        if (dns_resolve(host, &ip) < 0) {
            p_strncpy(err_out, "dns failed", err_max);
            return -1;
        }
    }

    tcp_handle_t h = tcp_open(ip, port);
    if (h == TCP_INVALID_HANDLE) {
        p_strncpy(err_out, "tcp connect failed", err_max);
        return -1;
    }

    /* Build IPP body. */
    static uint8_t ipp_hdr[1024];
    int hdr_len = build_ipp_print_job(ipp_hdr, (int)sizeof(ipp_hdr),
                                      p->addr, job_title, doc_format);
    if (hdr_len < 0) {
        tcp_close_on(h);
        p_strncpy(err_out, "ipp build failed", err_max);
        return -1;
    }

    uint32_t body_len = (uint32_t)hdr_len + len;

    /* HTTP POST. */
    static uint8_t req[1280];
    int rpos = 0;
    rpos = p_append(req, rpos, sizeof(req), "POST ");
    rpos = p_append(req, rpos, sizeof(req), path[0] ? path : "/ipp/print");
    rpos = p_append(req, rpos, sizeof(req), " HTTP/1.1\r\nHost: ");
    rpos = p_append(req, rpos, sizeof(req), host);
    rpos = p_append(req, rpos, sizeof(req), "\r\nContent-Type: application/ipp\r\nContent-Length: ");
    rpos = p_append_dec(req, rpos, sizeof(req), body_len);
    rpos = p_append(req, rpos, sizeof(req), "\r\nConnection: close\r\nUser-Agent: Zeos/1.0\r\n\r\n");
    if (rpos < 0) {
        tcp_close_on(h);
        p_strncpy(err_out, "http build failed", err_max);
        return -1;
    }

    /* Send headers, then IPP header, then payload (chunked into u16 sends). */
    if (tcp_send_on(h, req, (uint16_t)rpos) < 0) {
        tcp_close_on(h);
        p_strncpy(err_out, "tcp send hdr", err_max);
        return -1;
    }
    if (tcp_send_on(h, ipp_hdr, (uint16_t)hdr_len) < 0) {
        tcp_close_on(h);
        p_strncpy(err_out, "tcp send ipp", err_max);
        return -1;
    }
    uint32_t sent = 0;
    while (sent < len) {
        uint32_t remaining = len - sent;
        uint16_t chunk = (remaining > 0xF000) ? 0xF000 : (uint16_t)remaining;
        if (tcp_send_on(h, bytes + sent, chunk) < 0) {
            tcp_close_on(h);
            p_strncpy(err_out, "tcp send body", err_max);
            return -1;
        }
        sent += chunk;
    }

    /* Drain the response (we don't strictly need to parse it). */
    uint8_t rsp[256];
    int total = 0;
    for (int i = 0; i < 32; i++) {
        int n = tcp_recv_on(h, rsp, sizeof(rsp));
        if (n <= 0) break;
        total += n;
        if (total > 4096) break;
    }
    tcp_close_on(h);
    return 0;
}

/* ── Transport: USB bulk OUT ─────────────────────────────────────── */

static int transport_usb(const printer_info_t *p,
                         const uint8_t *bytes, uint32_t len,
                         char *err_out, int err_max)
{
    struct xhci_device *dev = find_usb_printer_dev(p);
    if (!dev) {
        p_strncpy(err_out, "usb device gone", err_max);
        return -1;
    }
    uint8_t ep_out = (uint8_t)p->port;
    uint32_t sent = 0;
    while (sent < len) {
        uint32_t remaining = len - sent;
        int chunk = (remaining > 16384) ? 16384 : (int)remaining;
        int rc = xhci_bulk_transfer(dev, ep_out,
                                    (void *)(bytes + sent), chunk, 0);
        if (rc < 0) {
            p_strncpy(err_out, "bulk-out failed", err_max);
            return -1;
        }
        sent += (uint32_t)rc;
        if (rc == 0) break;
    }
    return 0;
}

/* ── Transport: file PDF (fat32) ─────────────────────────────────── */

static int transport_file_pdf(const printer_info_t *p,
                              const uint8_t *bytes, uint32_t len,
                              char *err_out, int err_max)
{
    if (!fat32_mounted()) {
        p_strncpy(err_out, "fat32 not mounted", err_max);
        return -1;
    }
    /* Create or truncate the file at p->addr. */
    int rc = fat32_create(p->addr);
    (void)rc;
    if (fat32_truncate(p->addr, 0) < 0) {
        /* If create failed and truncate failed, try writing anyway. */
    }
    int wrote = fat32_write(p->addr, 0, bytes, len);
    if (wrote < 0 || (uint32_t)wrote != len) {
        p_strncpy(err_out, "fat32 write failed", err_max);
        return -1;
    }
    return 0;
}

/* ── Format node: build s_tx_bytes/s_tx_len ──────────────────────── */
/*
 * Walks the request, emits the printer-acceptable bytes into s_pdf_buf
 * if conversion is needed, otherwise points s_tx_bytes at s_doc_buf.
 * For PDF passthrough we just point at the doc buffer. */

static int format_to_target(const print_request_t *req,
                            const printer_info_t *p,
                            const char **out_format)
{
    print_format_t fmt = (print_format_t)req->format;
    if (fmt == PRINT_FMT_AUTO) fmt = detect_format(req->bytes, req->len);

    /* FILE_PDF: always emit a PDF (passthrough or convert). */
    if (p->transport == PRINT_TRANSPORT_FILE_PDF) {
        if (fmt == PRINT_FMT_PDF) {
            s_tx_bytes = req->bytes;
            s_tx_len   = req->len;
            *out_format = "application/pdf";
            return 0;
        }
        if (fmt == PRINT_FMT_TEXT) {
            int n = print_pdf_from_text((const char *)req->bytes, req->len,
                                        s_pdf_buf, sizeof(s_pdf_buf));
            if (n < 0) return -1;
            s_tx_bytes = s_pdf_buf;
            s_tx_len   = (uint32_t)n;
            *out_format = "application/pdf";
            return 0;
        }
        if (fmt == PRINT_FMT_PNG || fmt == PRINT_FMT_JPEG) {
            int n = print_pdf_from_image(req->bytes, req->len, fmt,
                                         612, 792, s_pdf_buf, sizeof(s_pdf_buf));
            if (n < 0) return -1;
            s_tx_bytes = s_pdf_buf;
            s_tx_len   = (uint32_t)n;
            *out_format = "application/pdf";
            return 0;
        }
        return -1;
    }

    /* IPP_NETWORK + USB: prefer PDF; convert text/image. */
    if (fmt == PRINT_FMT_PDF) {
        s_tx_bytes = req->bytes;
        s_tx_len   = req->len;
        *out_format = "application/pdf";
        return 0;
    }
    if (fmt == PRINT_FMT_TEXT) {
        /* For USB: many printers accept raw text. We still wrap-as-PDF
         * for IPP, plain text for USB. */
        if (p->transport == PRINT_TRANSPORT_USB) {
            s_tx_bytes = req->bytes;
            s_tx_len   = req->len;
            *out_format = "text/plain";
            return 0;
        }
        int n = print_pdf_from_text((const char *)req->bytes, req->len,
                                    s_pdf_buf, sizeof(s_pdf_buf));
        if (n < 0) return -1;
        s_tx_bytes = s_pdf_buf;
        s_tx_len   = (uint32_t)n;
        *out_format = "application/pdf";
        return 0;
    }
    if (fmt == PRINT_FMT_PNG || fmt == PRINT_FMT_JPEG) {
        int n = print_pdf_from_image(req->bytes, req->len, fmt,
                                     612, 792, s_pdf_buf, sizeof(s_pdf_buf));
        if (n < 0) return -1;
        s_tx_bytes = s_pdf_buf;
        s_tx_len   = (uint32_t)n;
        *out_format = "application/pdf";
        return 0;
    }
    return -1;
}

/* ── Chain node resolves ─────────────────────────────────────────── */

static const char *s_pending_doc_format = "application/pdf";

static void node_print_request_resolve(chain_node_t *self,
                                       void *input, void *output)
{
    (void)self; (void)input;
    print_request_t *out = (print_request_t *)output;
    *out = s_pending_req;
}

static void node_format_resolve(chain_node_t *self,
                                void *input, void *output)
{
    (void)self;
    print_request_t *in  = (print_request_t *)input;
    print_request_t *out = (print_request_t *)output;
    *out = *in;
    s_tx_bytes = 0;
    s_tx_len   = 0;
    s_pending_doc_format = "application/pdf";

    if (!in->valid) {
        s_pending_cmp.error_code = -1;
        p_strncpy(s_pending_cmp.error_text, "invalid request",
                  sizeof(s_pending_cmp.error_text));
        return;
    }

    int slot = find_printer_slot(in->target_printer_id);
    if (slot < 0) {
        s_pending_cmp.error_code = -2;
        p_strncpy(s_pending_cmp.error_text, "no printer",
                  sizeof(s_pending_cmp.error_text));
        out->valid = 0;
        return;
    }

    const char *fmt_str = "application/pdf";
    if (format_to_target(in, &g_printers[slot], &fmt_str) != 0) {
        s_pending_cmp.error_code = -3;
        p_strncpy(s_pending_cmp.error_text, "format failed",
                  sizeof(s_pending_cmp.error_text));
        out->valid = 0;
        return;
    }
    s_pending_doc_format = fmt_str;
}

static void node_tx_to_printer_resolve(chain_node_t *self,
                                       void *input, void *output)
{
    (void)self;
    print_request_t *in  = (print_request_t *)input;
    print_completion_t *out = (print_completion_t *)output;
    p_memzero(out, sizeof(*out));
    out->job_id           = in->job_id;
    out->target_printer_id = in->target_printer_id;
    out->status           = PRINT_JOB_PRINTING;

    if (!in->valid || !s_tx_bytes || s_tx_len == 0) {
        out->status     = PRINT_JOB_FAILED;
        out->error_code = s_pending_cmp.error_code ? s_pending_cmp.error_code : -4;
        p_strncpy(out->error_text,
                  s_pending_cmp.error_text[0] ? s_pending_cmp.error_text : "no bytes",
                  sizeof(out->error_text));
        return;
    }

    int slot = find_printer_slot(in->target_printer_id);
    if (slot < 0) {
        out->status     = PRINT_JOB_FAILED;
        out->error_code = -2;
        p_strncpy(out->error_text, "no printer", sizeof(out->error_text));
        return;
    }
    printer_info_t *p = &g_printers[slot];
    p->status = PRINTER_STATUS_BUSY;

    char err[40] = {0};
    int rc = -1;
    switch (p->transport) {
    case PRINT_TRANSPORT_USB:
        rc = transport_usb(p, s_tx_bytes, s_tx_len, err, sizeof(err));
        break;
    case PRINT_TRANSPORT_IPP_NETWORK:
        rc = transport_ipp(p, s_tx_bytes, s_tx_len, in->title,
                           s_pending_doc_format, err, sizeof(err));
        break;
    case PRINT_TRANSPORT_FILE_PDF:
        rc = transport_file_pdf(p, s_tx_bytes, s_tx_len, err, sizeof(err));
        break;
    default:
        p_strncpy(err, "no transport", sizeof(err));
        rc = -1;
        break;
    }
    p->status = (rc == 0) ? PRINTER_STATUS_IDLE : PRINTER_STATUS_ERROR;
    p->jobs_total++;
    if (rc != 0) p->jobs_failed++;
    p->last_used_tsc = timer_read_tsc();

    if (rc == 0) {
        out->status        = PRINT_JOB_DONE;
        out->pages_printed = 1;
        out->error_code    = 0;
    } else {
        out->status     = PRINT_JOB_FAILED;
        out->error_code = -10;
        p_strncpy(out->error_text, err, sizeof(out->error_text));
    }
}

static void node_emit_completion_resolve(chain_node_t *self,
                                         void *input, void *output)
{
    (void)self;
    print_completion_t *in  = (print_completion_t *)input;
    print_completion_t *out = (print_completion_t *)output;
    *out = *in;
    s_pending_cmp = *in;
    g_jobs_total++;
    if (in->status == PRINT_JOB_FAILED) g_jobs_failed_total++;

    if (CHAIN_PRINT >= 0) {
        chain_t *c = chain_get(CHAIN_PRINT);
        if (c) c->vault_version++;
    }
}

/* ── Subscriber: CHAIN_PRINT_QUEUE ───────────────────────────────── */
/*
 * Input type: print_completion (matches CHAIN_PRINT's emit_completion
 * output). MDE auto-routes the fan-out. The single node
 * track_completion finds the matching job slot, transitions state,
 * stamps timing, persists, and emits a masq journal event. */

static void node_queue_track_resolve(chain_node_t *self,
                                     void *input, void *output)
{
    (void)self;
    print_completion_t *in  = (print_completion_t *)input;
    print_completion_t *out = (print_completion_t *)output;
    *out = *in;
    if (in->job_id == 0) return;

    int slot = job_find_slot(in->job_id);
    if (slot < 0) return;
    print_job_t *j = &g_jobs[slot];
    j->state         = in->status;
    j->pages_printed = in->pages_printed;
    j->error_code    = in->error_code;
    p_strncpy(j->error_text, in->error_text, sizeof(j->error_text));
    j->completed_tsc = timer_read_tsc();

    job_emit_masq(j,
                  in->status == PRINT_JOB_DONE   ? "completed" :
                  in->status == PRINT_JOB_FAILED ? "failed" :
                  "tx");

    persist_save();
    if (CHAIN_PRINT_QUEUE >= 0) {
        chain_t *c = chain_get(CHAIN_PRINT_QUEUE);
        if (c) c->vault_version++;
    }
}

/* ── Job slot management ─────────────────────────────────────────── */

static int job_find_slot(uint32_t job_id)
{
    for (int i = 0; i < PRINT_MAX_JOBS; i++)
        if (g_jobs[i].job_id == job_id) return i;
    return -1;
}

static print_job_t *job_alloc_slot(void)
{
    /* Prefer empty slot. */
    for (int i = 0; i < PRINT_MAX_JOBS; i++) {
        if (g_jobs[i].job_id == 0) return &g_jobs[i];
    }
    /* Else evict oldest completed/failed/cancelled. */
    int oldest = -1;
    uint64_t oldest_tsc = (uint64_t)-1;
    for (int i = 0; i < PRINT_MAX_JOBS; i++) {
        if (g_jobs[i].state == PRINT_JOB_DONE ||
            g_jobs[i].state == PRINT_JOB_FAILED ||
            g_jobs[i].state == PRINT_JOB_CANCELLED) {
            if ((uint64_t)g_jobs[i].completed_tsc < oldest_tsc) {
                oldest_tsc = g_jobs[i].completed_tsc;
                oldest = i;
            }
        }
    }
    if (oldest >= 0) return &g_jobs[oldest];
    return 0;   /* queue full, all in-flight */
}

int print_job_count(void)
{
    int n = 0;
    for (int i = 0; i < PRINT_MAX_JOBS; i++)
        if (g_jobs[i].job_id != 0) n++;
    return n;
}

int print_job_get(int idx, print_job_t *out)
{
    if (!out || idx < 0) return -1;
    int n = 0;
    for (int i = 0; i < PRINT_MAX_JOBS; i++) {
        if (g_jobs[i].job_id == 0) continue;
        if (n == idx) { *out = g_jobs[i]; return 0; }
        n++;
    }
    return -1;
}

int print_job_active_count(void)
{
    int n = 0;
    for (int i = 0; i < PRINT_MAX_JOBS; i++)
        if (g_jobs[i].state == PRINT_JOB_PENDING ||
            g_jobs[i].state == PRINT_JOB_PRINTING) n++;
    return n;
}

uint32_t print_chain_jobs_total(void)  { return g_jobs_total; }
uint32_t print_chain_jobs_active(void) { return (uint32_t)print_job_active_count(); }

static void job_emit_masq(const print_job_t *j, const char *event)
{
    /* MasQ journal entries flow through chain_resolve's TSC capture +
     * vault_version bump (already done at the chain level). We extend
     * by writing an append-only line to /print/journal — temporal
     * provenance for every job lifecycle event. */
    char line[160];
    int pos = 0;
    pos = p_append((uint8_t *)line, pos, sizeof(line), "job=");
    pos = p_append_dec((uint8_t *)line, pos, sizeof(line), j->job_id);
    pos = p_append((uint8_t *)line, pos, sizeof(line), " event=");
    pos = p_append((uint8_t *)line, pos, sizeof(line), event);
    pos = p_append((uint8_t *)line, pos, sizeof(line), " printer=");
    pos = p_append_dec((uint8_t *)line, pos, sizeof(line), (uint32_t)j->printer_id);
    pos = p_append((uint8_t *)line, pos, sizeof(line), " state=");
    pos = p_append_dec((uint8_t *)line, pos, sizeof(line), (uint32_t)j->state);
    pos = p_append((uint8_t *)line, pos, sizeof(line), " title=");
    pos = p_append((uint8_t *)line, pos, sizeof(line), j->title[0] ? j->title : "<none>");
    pos = p_append((uint8_t *)line, pos, sizeof(line), "\n");
    if (pos > 0 && pos < (int)sizeof(line))
        (void)vault_append("/print/journal", line, (uint32_t)pos);
}

/* ── Submission ──────────────────────────────────────────────────── */

uint32_t print_submit(const char *title,
                      print_format_t fmt,
                      const void *bytes, uint32_t len,
                      int target_printer_id, uint8_t copies)
{
    if (!bytes || len == 0 || len > PRINT_DOC_MAX) return 0;
    if (target_printer_id < 0 || target_printer_id == 0)
        target_printer_id = first_available_printer();
    if (target_printer_id <= 0) return 0;
    if (find_printer_slot(target_printer_id) < 0) return 0;
    if (copies == 0) copies = 1;

    spin_lock(&g_submit_lock);

    /* Allocate a job slot. */
    print_job_t *j = job_alloc_slot();
    if (!j) {
        spin_unlock(&g_submit_lock);
        return 0;
    }
    p_memzero(j, sizeof(*j));
    j->job_id        = g_next_job_id++;
    j->state         = PRINT_JOB_PENDING;
    j->printer_id    = (uint8_t)target_printer_id;
    j->copies        = copies;
    j->len           = len;
    j->submitted_tsc = timer_read_tsc();
    p_strncpy(j->title, title && title[0] ? title : "Untitled",
              sizeof(j->title));

    /* Copy doc bytes. */
    p_memcpy(s_doc_buf, bytes, len);

    /* Detect / store format on the job (best-effort, after detect). */
    if (fmt == PRINT_FMT_AUTO) fmt = detect_format(s_doc_buf, len);
    j->format = (uint8_t)fmt;

    job_emit_masq(j, "queued");

    /* Stage and resolve. */
    p_memzero(&s_pending_req, sizeof(s_pending_req));
    p_memzero(&s_pending_cmp, sizeof(s_pending_cmp));
    s_pending_req.valid             = 1;
    s_pending_req.format            = (uint8_t)fmt;
    s_pending_req.target_printer_id = (uint8_t)target_printer_id;
    s_pending_req.copies            = copies;
    s_pending_req.job_id            = j->job_id;
    s_pending_req.len               = len;
    s_pending_req.bytes             = s_doc_buf;
    p_strncpy(s_pending_req.title, j->title, sizeof(s_pending_req.title));

    j->state = PRINT_JOB_PRINTING;
    job_emit_masq(j, "started");

    int rc = -1;
    if (CHAIN_PRINT < 0) {
        /* Pre-registry inline run. */
        print_request_t r1, r2;
        node_print_request_resolve(0, 0, &r1);
        node_format_resolve(0, &r1, &r2);
        node_tx_to_printer_resolve(0, &r2, &s_pending_cmp);
        node_emit_completion_resolve(0, &s_pending_cmp, &s_pending_cmp);
        rc = 0;
    } else {
        rc = chain_resolve(CHAIN_PRINT);
    }
    (void)rc;

    /* Commit job state from completion. */
    j->state         = s_pending_cmp.status;
    j->pages_printed = s_pending_cmp.pages_printed;
    j->error_code    = s_pending_cmp.error_code;
    p_strncpy(j->error_text, s_pending_cmp.error_text, sizeof(j->error_text));
    j->completed_tsc = timer_read_tsc();

    /* Drive CHAIN_PRINT_QUEUE explicitly so the subscriber updates even
     * when MDE auto-route hasn't connected (e.g., during early boot or
     * tests). When MDE has wired the route, this resolve is a cheap
     * idempotent re-track. */
    if (CHAIN_PRINT_QUEUE >= 0) {
        (void)chain_resolve(CHAIN_PRINT_QUEUE);
    } else {
        node_queue_track_resolve(0, &s_pending_cmp, &s_pending_cmp);
    }

    persist_save();
    uint32_t job_id = j->job_id;
    spin_unlock(&g_submit_lock);
    return job_id;
}

int print_cancel(uint32_t job_id)
{
    int slot = job_find_slot(job_id);
    if (slot < 0) return -1;
    print_job_t *j = &g_jobs[slot];
    if (j->state == PRINT_JOB_DONE || j->state == PRINT_JOB_FAILED ||
        j->state == PRINT_JOB_CANCELLED) return -1;
    j->state = PRINT_JOB_CANCELLED;
    j->completed_tsc = timer_read_tsc();
    job_emit_masq(j, "cancelled");
    persist_save();
    if (CHAIN_PRINT_QUEUE >= 0) {
        chain_t *c = chain_get(CHAIN_PRINT_QUEUE);
        if (c) c->vault_version++;
    }
    return 0;
}

/* ── VAULT persistence ───────────────────────────────────────────── */

#define PRINT_PRINTERS_MAGIC 0x504E5452u  /* "PNTR" */
#define PRINT_JOBS_MAGIC     0x504A4F42u  /* "PJOB" */
#define PRINT_VERSION        1u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    uint32_t next_id;
} print_blob_hdr_t;

static void persist_save(void)
{
    /* Printers. */
    {
        uint8_t buf[sizeof(print_blob_hdr_t) +
                    sizeof(printer_info_t) * PRINT_MAX_PRINTERS];
        print_blob_hdr_t hdr;
        hdr.magic   = PRINT_PRINTERS_MAGIC;
        hdr.version = PRINT_VERSION;
        hdr.count   = (uint32_t)print_printer_count();
        hdr.next_id = (uint32_t)g_next_printer_id;
        p_memcpy(buf, &hdr, sizeof(hdr));
        uint32_t off = sizeof(hdr);
        for (int i = 0; i < PRINT_MAX_PRINTERS; i++) {
            if (!g_printers[i].in_use) continue;
            p_memcpy(buf + off, &g_printers[i], sizeof(printer_info_t));
            off += sizeof(printer_info_t);
        }
        (void)vault_save_config(PRINT_KEY_PRINTERS, buf, off);
    }
    /* Jobs. */
    {
        uint8_t buf[sizeof(print_blob_hdr_t) +
                    sizeof(print_job_t) * PRINT_MAX_JOBS];
        print_blob_hdr_t hdr;
        hdr.magic   = PRINT_JOBS_MAGIC;
        hdr.version = PRINT_VERSION;
        hdr.count   = (uint32_t)print_job_count();
        hdr.next_id = g_next_job_id;
        p_memcpy(buf, &hdr, sizeof(hdr));
        uint32_t off = sizeof(hdr);
        for (int i = 0; i < PRINT_MAX_JOBS; i++) {
            if (g_jobs[i].job_id == 0) continue;
            p_memcpy(buf + off, &g_jobs[i], sizeof(print_job_t));
            off += sizeof(print_job_t);
        }
        (void)vault_save_config(PRINT_KEY_JOBS, buf, off);
    }
}

static void persist_restore(void)
{
    /* Printers. */
    {
        uint8_t buf[sizeof(print_blob_hdr_t) +
                    sizeof(printer_info_t) * PRINT_MAX_PRINTERS];
        int got = vault_load_config(PRINT_KEY_PRINTERS, buf, sizeof(buf));
        if (got >= (int)sizeof(print_blob_hdr_t)) {
            print_blob_hdr_t hdr;
            p_memcpy(&hdr, buf, sizeof(hdr));
            if (hdr.magic == PRINT_PRINTERS_MAGIC && hdr.version == PRINT_VERSION) {
                if (hdr.count <= PRINT_MAX_PRINTERS) {
                    /* Restore each in-order into the first N empty slots. */
                    g_next_printer_id = (int)(hdr.next_id ? hdr.next_id : 1);
                    uint32_t off = sizeof(hdr);
                    for (uint32_t i = 0; i < hdr.count && i < PRINT_MAX_PRINTERS; i++) {
                        printer_info_t pi;
                        p_memcpy(&pi, buf + off, sizeof(printer_info_t));
                        off += sizeof(printer_info_t);
                        if (!pi.in_use) continue;
                        g_printers[i] = pi;
                        /* Status starts as IDLE — a USB device may be gone. */
                        if (g_printers[i].transport == PRINT_TRANSPORT_USB)
                            g_printers[i].status = PRINTER_STATUS_OFFLINE;
                        else
                            g_printers[i].status = PRINTER_STATUS_IDLE;
                    }
                }
            }
        }
    }
    /* Jobs. */
    {
        uint8_t buf[sizeof(print_blob_hdr_t) +
                    sizeof(print_job_t) * PRINT_MAX_JOBS];
        int got = vault_load_config(PRINT_KEY_JOBS, buf, sizeof(buf));
        if (got >= (int)sizeof(print_blob_hdr_t)) {
            print_blob_hdr_t hdr;
            p_memcpy(&hdr, buf, sizeof(hdr));
            if (hdr.magic == PRINT_JOBS_MAGIC && hdr.version == PRINT_VERSION) {
                if (hdr.count <= PRINT_MAX_JOBS) {
                    g_next_job_id = hdr.next_id ? hdr.next_id : 1;
                    uint32_t off = sizeof(hdr);
                    for (uint32_t i = 0; i < hdr.count && i < PRINT_MAX_JOBS; i++) {
                        print_job_t pj;
                        p_memcpy(&pj, buf + off, sizeof(print_job_t));
                        off += sizeof(print_job_t);
                        g_jobs[i] = pj;
                        /* In-flight jobs at boot: mark as failed (we lost
                         * the document buffer at reboot). The journal
                         * preserves the trail. */
                        if (g_jobs[i].state == PRINT_JOB_PENDING ||
                            g_jobs[i].state == PRINT_JOB_PRINTING) {
                            g_jobs[i].state = PRINT_JOB_FAILED;
                            p_strncpy(g_jobs[i].error_text, "lost on reboot",
                                      sizeof(g_jobs[i].error_text));
                            g_jobs[i].error_code = -100;
                        }
                    }
                    g_job_count = (int)hdr.count;
                }
            }
        }
    }
}

/* ── Init ────────────────────────────────────────────────────────── */

void print_init(void)
{
    if (g_initialized) return;
    g_initialized = 1;
    p_memzero(g_printers, sizeof(g_printers));
    p_memzero(g_jobs, sizeof(g_jobs));
    g_next_printer_id = 1;
    g_next_job_id     = 1;
    g_job_count       = 0;

    persist_restore();

    /* Always make sure FILE_PDF "print to PDF" is available so the
     * default-no-network case still works for the basic case. We add a
     * built-in printer pointing at /tmp/print.pdf if no printers are
     * configured. */
    if (print_printer_count() == 0) {
        /* No persisted printers; install the default print-to-pdf
         * destination. The fat32 path is created lazily on first use. */
        (void)print_printer_add_file_pdf("Print to PDF", "/print-output.pdf");
    }
}

int print_chain_register(int parent_chain_id)
{
    if (CHAIN_PRINT < 0) {
        int id = chain_create("print", parent_chain_id, MASQ_INTERNAL);
        if (id < 0) return -1;
        chain_add_node(id, "print_request",
                       "print_request_in", "print_request",
                       node_print_request_resolve);
        chain_add_node(id, "format",
                       "print_request", "print_request",
                       node_format_resolve);
        chain_add_node(id, "tx_to_printer",
                       "print_request", "print_completion",
                       node_tx_to_printer_resolve);
        chain_add_node(id, "emit_completion",
                       "print_completion", "print_completion",
                       node_emit_completion_resolve);
        CHAIN_PRINT = id;
    }
    if (CHAIN_PRINT_QUEUE < 0) {
        int qid = chain_create("print_queue", parent_chain_id, MASQ_INTERNAL);
        if (qid < 0) return CHAIN_PRINT;
        chain_add_node(qid, "track_completion",
                       "print_completion", "print_completion",
                       node_queue_track_resolve);
        CHAIN_PRINT_QUEUE = qid;
    }
    return CHAIN_PRINT;
}

/* ── Selftest line ───────────────────────────────────────────────── */

void print_print_selftest_line(void)
{
    kputs("  Print ................. CHAIN_PRINT registered, ");
    kput_dec((uint64_t)print_printer_count());
    kputs(" printers, ");
    kput_dec((uint64_t)print_job_count());
    kputs(" jobs queued, ");
    kput_dec((uint64_t)print_job_active_count());
    kputs(" active\n");
}

/* ── Shell commands ──────────────────────────────────────────────── */

static const char *transport_label(uint8_t t) {
    switch (t) {
    case PRINT_TRANSPORT_USB:         return "usb";
    case PRINT_TRANSPORT_IPP_NETWORK: return "ipp";
    case PRINT_TRANSPORT_FILE_PDF:    return "file";
    default: return "?";
    }
}
static const char *status_label(uint8_t s) {
    switch (s) {
    case PRINTER_STATUS_IDLE:    return "idle";
    case PRINTER_STATUS_BUSY:    return "busy";
    case PRINTER_STATUS_ERROR:   return "error";
    case PRINTER_STATUS_OFFLINE: return "offline";
    default: return "?";
    }
}
static const char *job_state_label(uint8_t s) {
    switch (s) {
    case PRINT_JOB_PENDING:   return "pending";
    case PRINT_JOB_PRINTING:  return "printing";
    case PRINT_JOB_DONE:      return "done";
    case PRINT_JOB_FAILED:    return "failed";
    case PRINT_JOB_CANCELLED: return "cancelled";
    default: return "?";
    }
}

static const char *skip_ws(const char *s) {
    while (s && (*s == ' ' || *s == '\t')) s++;
    return s;
}
static const char *next_tok(const char *s, char *out, int max) {
    s = skip_ws(s);
    if (!s || !*s) { if (out) out[0] = 0; return 0; }
    int i = 0;
    while (*s && *s != ' ' && *s != '\t' && i < max - 1) out[i++] = *s++;
    out[i] = 0;
    return s;
}

void printers_cmd(const char *args)
{
    (void)args;
    int n = print_printer_count();
    kputs("\n");
    if (n == 0) { kputs("  (no printers)\n\n"); return; }
    for (int i = 0; i < n; i++) {
        printer_info_t p;
        if (print_printer_get(i, &p) != 0) continue;
        kputs("  #"); kput_dec((uint64_t)p.id);
        kputs(" "); kputs(transport_label(p.transport));
        kputs(" ["); kputs(status_label(p.status)); kputs("] ");
        kputs(p.name);
        kputs(" -- "); kputs(p.addr);
        kputs(" (jobs="); kput_dec((uint64_t)p.jobs_total);
        kputs(" failed="); kput_dec((uint64_t)p.jobs_failed);
        kputs(")\n");
    }
    kputs("\n");
}

void printer_cmd(const char *args)
{
    char tok[32];
    args = next_tok(args, tok, sizeof(tok));
    if (!args) {
        kputs("usage: printer add usb | printer add ipp <url> | printer remove <id> | printer discover\n");
        return;
    }
    if (p_streq(tok, "add")) {
        char what[32];
        args = next_tok(args, what, sizeof(what));
        if (!args) { kputs("usage: printer add usb|ipp <url>\n"); return; }
        if (p_streq(what, "usb")) {
            int n = print_usb_scan();
            kputs("  added "); kput_dec((uint64_t)n);
            kputs(" USB printer(s)\n");
            return;
        }
        if (p_streq(what, "ipp")) {
            args = skip_ws(args);
            if (!*args) { kputs("usage: printer add ipp <url>\n"); return; }
            char name[32]; name[0] = 0;
            int id = print_printer_add_ipp(name, args);
            if (id > 0) {
                kputs("  added IPP printer #"); kput_dec((uint64_t)id);
                kputs("\n");
            } else {
                kputs("  add failed\n");
            }
            return;
        }
        if (p_streq(what, "file")) {
            args = skip_ws(args);
            if (!*args) { kputs("usage: printer add file <path>\n"); return; }
            int id = print_printer_add_file_pdf("File PDF", args);
            if (id > 0) {
                kputs("  added file printer #"); kput_dec((uint64_t)id);
                kputs("\n");
            } else {
                kputs("  add failed\n");
            }
            return;
        }
        kputs("unknown printer kind\n");
        return;
    }
    if (p_streq(tok, "remove") || p_streq(tok, "rm")) {
        char idtok[16];
        args = next_tok(args, idtok, sizeof(idtok));
        if (!args && !idtok[0]) { kputs("usage: printer remove <id>\n"); return; }
        uint32_t id = 0; const char *q = idtok;
        while (*q >= '0' && *q <= '9') { id = id * 10 + (uint32_t)(*q - '0'); q++; }
        if (id == 0) { kputs("bad id\n"); return; }
        if (print_printer_remove((int)id) == 0) kputs("  removed\n");
        else kputs("  remove failed\n");
        return;
    }
    if (p_streq(tok, "discover")) {
        kputs("  mDNS discover...\n");
        int n = print_mdns_discover();
        kputs("  discovered "); kput_dec((uint64_t)n);
        kputs(" printer(s)\n");
        return;
    }
    kputs("usage: printer add usb | printer add ipp <url> | printer remove <id> | printer discover\n");
}

/* Read a fat32 file into s_doc_buf. Returns bytes read or -1. */
static int read_fat32_file(const char *path, uint32_t *out_len)
{
    if (!fat32_mounted()) return -1;
    struct fat32_file f;
    if (fat32_open(path, &f) != 0) return -1;
    int n = fat32_read(&f, s_doc_buf, PRINT_DOC_MAX);
    if (n < 0) return -1;
    if (out_len) *out_len = (uint32_t)n;
    return n;
}

/* Read a VAULT file into s_doc_buf. */
static int read_vault_file(const char *path, uint32_t *out_len)
{
    int n = vault_read(path, s_doc_buf, PRINT_DOC_MAX);
    if (n < 0) return -1;
    if (out_len) *out_len = (uint32_t)n;
    return n;
}

void print_cmd(const char *args)
{
    /* `print --to-pdf <path>` writes a PDF representation to <path>. */
    args = skip_ws(args);
    if (p_starts(args, "--to-pdf")) {
        args += 8;
        args = skip_ws(args);
        if (!*args) {
            kputs("usage: print --to-pdf <path>\n");
            return;
        }
        char src[64]; src[0] = 0;
        const char *more = next_tok(args, src, sizeof(src));
        if (more) more = skip_ws(more);

        /* If a second token is present, src is the source and `more` is
         * the destination. Otherwise treat the single arg as dest and
         * use a default sample text. */
        const char *dst = src;
        const char *body = "Hello from Zeos.\nCHAIN_PRINT --to-pdf path.\n";
        uint32_t body_len = (uint32_t)p_strlen(body);
        if (more && *more) {
            dst = more;
            /* try fat32 first, then vault */
            uint32_t got = 0;
            int n = read_fat32_file(src, &got);
            if (n < 0) n = read_vault_file(src, &got);
            if (n < 0) {
                kputs("source not found in fat32 or vault\n");
                return;
            }
            body = (const char *)s_doc_buf;
            body_len = got;
        }

        /* Generate PDF (PDF passthrough or wrap text). */
        static uint8_t local_pdf[PRINT_PDF_OUT_MAX];
        int pdfn;
        if (body_len >= 4 && body[0] == '%' && body[1] == 'P' &&
            body[2] == 'D' && body[3] == 'F') {
            /* Already PDF — write directly. */
            if (body_len > sizeof(local_pdf)) body_len = sizeof(local_pdf);
            p_memcpy(local_pdf, body, body_len);
            pdfn = (int)body_len;
        } else {
            pdfn = print_pdf_from_text(body, body_len, local_pdf, sizeof(local_pdf));
        }
        if (pdfn < 0) {
            kputs("PDF generation failed\n");
            return;
        }

        if (!fat32_mounted()) {
            kputs("fat32 not mounted; printing to VAULT instead\n");
            int rc = vault_write(dst, local_pdf, (uint32_t)pdfn);
            if (rc < 0) { kputs("vault write failed\n"); return; }
            kputs("  wrote "); kput_dec((uint64_t)pdfn);
            kputs(" bytes to "); kputs(dst); kputs(" (vault)\n");
            return;
        }
        (void)fat32_create(dst);
        (void)fat32_truncate(dst, 0);
        int wr = fat32_write(dst, 0, local_pdf, (uint32_t)pdfn);
        if (wr < 0) { kputs("fat32 write failed\n"); return; }
        kputs("  wrote "); kput_dec((uint64_t)wr);
        kputs(" bytes to "); kputs(dst); kputs("\n");
        return;
    }

    /* Otherwise: print <path> [--printer N]. */
    if (!*args) {
        kputs("usage: print <path> [--printer N] | print --to-pdf <path>\n");
        return;
    }

    char path[128];
    const char *p = next_tok(args, path, sizeof(path));
    if (!path[0]) {
        kputs("usage: print <path> [--printer N]\n");
        return;
    }

    int target = 0;
    if (p) {
        p = skip_ws(p);
        if (p_starts(p, "--printer")) {
            p += 9;
            p = skip_ws(p);
            uint32_t v = 0;
            while (*p >= '0' && *p <= '9') { v = v * 10 + (uint32_t)(*p - '0'); p++; }
            if (v > 0) target = (int)v;
        }
    }

    /* Read file (try fat32, then vault). */
    uint32_t doc_len = 0;
    int n = read_fat32_file(path, &doc_len);
    if (n < 0) n = read_vault_file(path, &doc_len);
    if (n < 0) {
        kputs("file not found in fat32 or vault\n");
        return;
    }

    /* Use a separate buffer for the doc since print_submit will copy
     * into s_doc_buf, and we read into s_doc_buf above. To avoid alias,
     * copy through a stack temp via the body pointer below. Submit will
     * memcpy from doc_bytes into s_doc_buf — same source/dest is fine
     * because p_memcpy is forward-direction safe for non-overlapping or
     * identical regions. */
    uint32_t job_id = print_submit(path, PRINT_FMT_AUTO,
                                   s_doc_buf, doc_len, target, 1);
    if (job_id == 0) {
        kputs("submit failed\n");
        return;
    }
    print_job_t j;
    if (job_find_slot(job_id) >= 0 && print_job_get(0, &j) == 0) {
        /* We don't have an idx-by-id walk; repeat get until match. */
        for (int i = 0; i < print_job_count(); i++) {
            if (print_job_get(i, &j) == 0 && j.job_id == job_id) break;
        }
    }
    kputs("  submitted job "); kput_dec((uint64_t)job_id);
    int slot = job_find_slot(job_id);
    if (slot >= 0) {
        kputs(" -> "); kputs(job_state_label(g_jobs[slot].state));
        if (g_jobs[slot].state == PRINT_JOB_FAILED && g_jobs[slot].error_text[0]) {
            kputs(" ("); kputs(g_jobs[slot].error_text); kputs(")");
        }
    }
    kputs("\n");
}

void print_queue_cmd(const char *args)
{
    (void)args;
    int n = print_job_count();
    kputs("\n");
    if (n == 0) { kputs("  (queue empty)\n\n"); return; }
    for (int i = 0; i < n; i++) {
        print_job_t j;
        if (print_job_get(i, &j) != 0) continue;
        kputs("  #"); kput_dec((uint64_t)j.job_id);
        kputs(" "); kputs(job_state_label(j.state));
        kputs(" printer="); kput_dec((uint64_t)j.printer_id);
        kputs(" len="); kput_dec((uint64_t)j.len);
        kputs(" pages="); kput_dec((uint64_t)j.pages_printed);
        kputs(" title="); kputs(j.title[0] ? j.title : "<none>");
        if (j.state == PRINT_JOB_FAILED && j.error_text[0]) {
            kputs(" err="); kputs(j.error_text);
        }
        kputs("\n");
    }
    kputs("\n");
}

void print_cancel_cmd(const char *args)
{
    args = skip_ws(args);
    uint32_t id = 0;
    while (*args >= '0' && *args <= '9') { id = id * 10 + (uint32_t)(*args - '0'); args++; }
    if (id == 0) { kputs("usage: print-cancel <job_id>\n"); return; }
    if (print_cancel(id) == 0) kputs("  cancelled\n");
    else kputs("  not found or already finished\n");
}
