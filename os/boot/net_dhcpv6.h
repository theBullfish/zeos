/*
 * Zeos -- DHCPv6 client (RFC 8415)
 *
 * Four-message dance: SOLICIT -> ADVERTISE -> REQUEST -> REPLY.
 * Only run when an RA has the M-bit set (managed configuration).
 */

#ifndef ZEOS_NET_DHCPV6_H
#define ZEOS_NET_DHCPV6_H

#include "net_ipv6.h"

#define DHCPV6_CLIENT_PORT  546
#define DHCPV6_SERVER_PORT  547

#define DHCP6_SOLICIT       1
#define DHCP6_ADVERTISE     2
#define DHCP6_REQUEST       3
#define DHCP6_REPLY         7
#define DHCP6_RENEW         5

#define DHCP6_OPT_CLIENTID  1
#define DHCP6_OPT_SERVERID  2
#define DHCP6_OPT_IA_NA     3
#define DHCP6_OPT_IAADDR    5
#define DHCP6_OPT_ORO       6
#define DHCP6_OPT_ELAPSED   8
#define DHCP6_OPT_DNS       23

struct dhcpv6_lease {
    struct ipv6_addr addr;
    uint32_t         valid_lifetime;
    uint32_t         preferred_lifetime;
    int              valid;
};

int dhcpv6_run(void);
const struct dhcpv6_lease *dhcpv6_get_lease(void);

#endif /* ZEOS_NET_DHCPV6_H */
