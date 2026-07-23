/*
 * Zeos — DHCP Client
 *
 * Implements DHCP DORA (Discover → Offer → Request → Ack)
 * to obtain IP, netmask, gateway, and DNS from the network.
 *
 * Replaces hardcoded QEMU SLIRP addresses with dynamic config.
 * Falls back to SLIRP defaults if DHCP fails after retries.
 */

#ifndef ZEOS_NET_DHCP_H
#define ZEOS_NET_DHCP_H

#include "net.h"

/* DHCP ports */
#define DHCP_CLIENT_PORT  68
#define DHCP_SERVER_PORT  67

/* DHCP message types */
#define DHCP_DISCOVER     1
#define DHCP_OFFER        2
#define DHCP_REQUEST      3
#define DHCP_DECLINE      4
#define DHCP_ACK          5
#define DHCP_NAK          6
#define DHCP_RELEASE      7

/* DHCP option codes */
#define DHCP_OPT_PAD           0
#define DHCP_OPT_SUBNET        1
#define DHCP_OPT_ROUTER        3
#define DHCP_OPT_DNS           6
#define DHCP_OPT_HOSTNAME     12
#define DHCP_OPT_DOMAIN       15
#define DHCP_OPT_BROADCAST    28
#define DHCP_OPT_REQUESTED_IP 50
#define DHCP_OPT_LEASE_TIME   51
#define DHCP_OPT_MSG_TYPE     53
#define DHCP_OPT_SERVER_ID    54
#define DHCP_OPT_PARAM_REQ    55
#define DHCP_OPT_END         255

/* DHCP magic cookie */
#define DHCP_MAGIC  0x63825363

/* DHCP message (fixed part: 236 bytes + options) */
struct dhcp_msg {
    uint8_t  op;             /* 1 = BOOTREQUEST, 2 = BOOTREPLY */
    uint8_t  htype;          /* 1 = Ethernet */
    uint8_t  hlen;           /* 6 = MAC length */
    uint8_t  hops;
    uint32_t xid;            /* Transaction ID */
    uint16_t secs;
    uint16_t flags;          /* 0x8000 = broadcast */
    uint32_t ciaddr;         /* Client IP (0 until bound) */
    uint32_t yiaddr;         /* Your IP (offered by server) */
    uint32_t siaddr;         /* Server IP */
    uint32_t giaddr;         /* Gateway IP (relay) */
    uint8_t  chaddr[16];     /* Client hardware address */
    uint8_t  sname[64];      /* Server hostname */
    uint8_t  file[128];      /* Boot filename */
    uint32_t magic;          /* DHCP magic cookie */
    uint8_t  options[312];   /* DHCP options */
} __attribute__((packed));

/* DHCP lease info (result of successful DORA) */
struct dhcp_lease {
    struct ipv4_addr ip;
    struct ipv4_addr netmask;
    struct ipv4_addr gateway;
    struct ipv4_addr dns;
    struct ipv4_addr server;     /* DHCP server that gave us this */
    uint32_t         lease_time; /* Seconds */
    int              valid;
};

/*
 * Run DHCP to obtain network configuration.
 * Populates g_net on success. Returns 0 on success, -1 on failure.
 * On failure, g_net retains fallback values (caller should set them).
 *
 * Tries up to 3 times with increasing timeout (1s, 2s, 4s).
 */
int dhcp_discover(void);

/* Get current lease info (for display/diagnostics) */
const struct dhcp_lease *dhcp_get_lease(void);

/* Async DHCP: non-blocking. dhcp_start() kicks it off; dhcp_service() advances
 * the state machine once per scheduler tick; dhcp_is_bound() reports success. */
void dhcp_start(void);
void dhcp_service(void);
int  dhcp_is_bound(void);

#endif /* ZEOS_NET_DHCP_H */
