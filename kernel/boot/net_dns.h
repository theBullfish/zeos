/*
 * Zeos — DNS Resolver
 */

#ifndef ZEOS_NET_DNS_H
#define ZEOS_NET_DNS_H

#include "net.h"
#include "net_ipv6.h"

/* Resolve a hostname to an IPv4 address. Blocking.
 * Returns 0 on success, -1 on failure. */
int dns_resolve(const char *hostname, struct ipv4_addr *out);

/* Resolve AAAA. */
int dns_resolve_aaaa(const char *hostname, struct ipv6_addr *out);

#endif /* ZEOS_NET_DNS_H */
