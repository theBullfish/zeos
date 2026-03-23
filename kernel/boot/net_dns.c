/*
 * Zeos — DNS Resolver
 *
 * Single A-record query via UDP to QEMU's SLIRP DNS (10.0.2.3:53).
 */

#include "net_dns.h"
#include "net_udp.h"
#include "kprint.h"

/* DNS cache */
#define DNS_CACHE_SIZE 8
static struct {
    char             hostname[64];
    struct ipv4_addr ip;
    int              valid;
} dns_cache[DNS_CACHE_SIZE];

/* DNS response state (set by callback) */
static volatile int dns_got_reply = 0;
static struct ipv4_addr dns_reply_ip;

/* DNS header */
struct dns_hdr {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} __attribute__((packed));

static int str_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

static void str_cpy(char *dst, const char *src, int max)
{
    int i;
    for (i = 0; i < max - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* Encode a hostname as DNS name format (length-prefixed labels) */
static int dns_encode_name(const char *hostname, uint8_t *buf, int max)
{
    int pos = 0;
    const char *p = hostname;

    while (*p && pos < max - 2) {
        /* Find next dot or end */
        const char *start = p;
        int label_len = 0;
        while (*p && *p != '.') {
            p++;
            label_len++;
        }

        if (label_len == 0 || label_len > 63)
            return -1;

        buf[pos++] = (uint8_t)label_len;
        for (int i = 0; i < label_len && pos < max; i++)
            buf[pos++] = (uint8_t)start[i];

        if (*p == '.') p++;
    }

    buf[pos++] = 0;  /* Root label */
    return pos;
}

/* DNS response callback */
static void dns_recv_callback(struct ipv4_addr src, uint16_t src_port,
                               const void *data, uint16_t len)
{
    (void)src;
    (void)src_port;

    if (len < sizeof(struct dns_hdr))
        return;

    const struct dns_hdr *hdr = (const struct dns_hdr *)data;
    uint16_t flags = ntohs(hdr->flags);
    uint16_t ancount = ntohs(hdr->ancount);

    /* Check: response, no error */
    if (!(flags & 0x8000))  return;  /* Not a response */
    if (flags & 0x000F)     return;  /* Error code */
    if (ancount == 0)       return;  /* No answers */

    /* Skip question section */
    const uint8_t *p = (const uint8_t *)data + sizeof(struct dns_hdr);
    const uint8_t *end = (const uint8_t *)data + len;

    /* Skip QNAME */
    while (p < end && *p != 0) {
        if ((*p & 0xC0) == 0xC0) { p += 2; goto past_qname; }
        p += *p + 1;
    }
    p++;  /* Skip null terminator */
past_qname:
    p += 4;  /* Skip QTYPE + QCLASS */

    /* Parse answer section — find first A record */
    for (uint16_t i = 0; i < ancount && p + 12 <= end; i++) {
        /* Skip NAME (might be compressed) */
        if ((*p & 0xC0) == 0xC0) {
            p += 2;
        } else {
            while (p < end && *p != 0) p += *p + 1;
            p++;
        }

        if (p + 10 > end) break;

        uint16_t rtype = ((uint16_t)p[0] << 8) | p[1];
        /* uint16_t rclass = ((uint16_t)p[2] << 8) | p[3]; */
        /* uint32_t ttl = ...; */
        uint16_t rdlength = ((uint16_t)p[8] << 8) | p[9];
        p += 10;

        if (rtype == 1 && rdlength == 4 && p + 4 <= end) {
            /* A record! */
            dns_reply_ip.b[0] = p[0];
            dns_reply_ip.b[1] = p[1];
            dns_reply_ip.b[2] = p[2];
            dns_reply_ip.b[3] = p[3];
            dns_got_reply = 1;
            return;
        }

        p += rdlength;
    }
}

static int dns_registered = 0;

int dns_resolve(const char *hostname, struct ipv4_addr *out)
{
    /* Check cache */
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (dns_cache[i].valid && str_eq(dns_cache[i].hostname, hostname)) {
            *out = dns_cache[i].ip;
            return 0;
        }
    }

    /* Register UDP callback on port 53 (our source port) */
    if (!dns_registered) {
        udp_bind(53, dns_recv_callback);  /* We'll use port 53 as source too */
        dns_registered = 1;
    }

    /* Build DNS query */
    uint8_t query[256];
    struct dns_hdr *hdr = (struct dns_hdr *)query;
    hdr->id = htons(0x1234);
    hdr->flags = htons(0x0100);  /* Standard query, recursion desired */
    hdr->qdcount = htons(1);
    hdr->ancount = 0;
    hdr->nscount = 0;
    hdr->arcount = 0;

    int name_len = dns_encode_name(hostname, query + sizeof(struct dns_hdr),
                                    256 - sizeof(struct dns_hdr) - 4);
    if (name_len < 0) return -1;

    int qpos = sizeof(struct dns_hdr) + name_len;
    query[qpos++] = 0; query[qpos++] = 1;   /* QTYPE: A */
    query[qpos++] = 0; query[qpos++] = 1;   /* QCLASS: IN */

    /* Send to DNS server */
    dns_got_reply = 0;
    udp_send(g_net.dns, 53, 53, query, (uint16_t)qpos);

    /* Wait for response (up to 3 seconds) */
    for (int i = 0; i < 30000 && !dns_got_reply; i++) {
        net_poll();
        for (volatile int j = 0; j < 5000; j++);
    }

    if (!dns_got_reply)
        return -1;

    *out = dns_reply_ip;

    /* Add to cache */
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (!dns_cache[i].valid) {
            str_cpy(dns_cache[i].hostname, hostname, 64);
            dns_cache[i].ip = dns_reply_ip;
            dns_cache[i].valid = 1;
            break;
        }
    }

    return 0;
}
