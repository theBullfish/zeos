/*
 * Zeos -- Stateful packet firewall (CHAIN_FIREWALL)
 *
 * Inserted into CHAIN_NET_TX/RX as a gate node. Default policy:
 * permissive outbound, restrictive inbound (drop unsolicited).
 * Connection table tracks established for stateful rules.
 *
 *   Pipeline: packet_inspect -> match_rule -> action -> emit_decision
 *
 * The hook nodes inside CHAIN_NET_TX/RX (firewall_check_tx /
 * firewall_check_rx) call firewall_check() which stages a
 * firewall_check_t and resolves CHAIN_FIREWALL. The decision comes
 * back via the staged firewall_decision_t. On DROP/REJECT the calling
 * chain truncates and emits a "dropped" tx_completion / delivery_status.
 */

#ifndef ZEOS_FIREWALL_H
#define ZEOS_FIREWALL_H

#include <stdint.h>
#include "net.h"

/* ── Limits ──────────────────────────────────────────────────────── */

#define FIREWALL_MAX_RULES        128
#define FIREWALL_CONN_MAX         256
#define FIREWALL_DROP_LOG_MAX     64

/* ── Direction ───────────────────────────────────────────────────── */

typedef enum {
    FW_DIR_IN   = 1,
    FW_DIR_OUT  = 2,
    FW_DIR_BOTH = 3,
} fw_dir_t;

/* ── Action ──────────────────────────────────────────────────────── */

typedef enum {
    FW_ACTION_ALLOW              = 0,
    FW_ACTION_DROP               = 1,
    FW_ACTION_REJECT             = 2,
    FW_ACTION_ALLOW_ESTABLISHED  = 3,
} fw_action_t;

/* ── L3/L4 match enums ───────────────────────────────────────────── */

typedef enum {
    FW_L3_ANY = 0,
    FW_L3_V4  = 1,
    FW_L3_V6  = 2,
} fw_l3_t;

typedef enum {
    FW_L4_ANY  = 0,
    FW_L4_TCP  = 6,
    FW_L4_UDP  = 17,
    FW_L4_ICMP = 1,
} fw_l4_t;

/* ── Connection state (TCP-ish) ──────────────────────────────────── */

typedef enum {
    FW_CONN_NEW         = 0,
    FW_CONN_ESTABLISHED = 1,
    FW_CONN_CLOSING     = 2,
} fw_conn_state_t;

/* ── Inspection input (to CHAIN_FIREWALL) ────────────────────────── */

typedef struct {
    uint8_t          direction;     /* FW_DIR_IN | FW_DIR_OUT */
    uint8_t          l3;            /* FW_L3_*  */
    uint8_t          l4;            /* FW_L4_*  */
    uint8_t          tcp_flags;     /* TCP only — FIN/SYN/RST/ACK */
    struct ipv4_addr src_ip;
    struct ipv4_addr dst_ip;
    uint16_t         src_port;
    uint16_t         dst_port;
    uint8_t          conn_state;    /* fw_conn_state_t after lookup */
    uint8_t          have_l4;       /* 1 if l4 header was successfully parsed */
    uint8_t          _pad[2];
    /* Frame pointer for use by REJECT path. NOT exposed to scratch. */
    const uint8_t   *frame;
    uint16_t         frame_len;
    uint8_t          _pad2[6];
} firewall_check_t;

/* ── Decision output ─────────────────────────────────────────────── */

typedef struct {
    uint8_t  action;        /* fw_action_t */
    uint8_t  log;
    uint8_t  _pad[2];
    int32_t  matched_rule_id;   /* -1 if default policy hit */
    char     reason[40];
} firewall_decision_t;

/* ── Rule ────────────────────────────────────────────────────────── */

typedef struct {
    uint8_t  enabled;
    uint8_t  direction;     /* fw_dir_t */
    uint8_t  l3;
    uint8_t  l4;
    uint16_t priority;      /* lower = checked first */
    uint16_t src_port_min;
    uint16_t src_port_max;
    uint16_t dst_port_min;
    uint16_t dst_port_max;
    struct ipv4_addr src_ip_match;
    struct ipv4_addr src_ip_mask;
    struct ipv4_addr dst_ip_match;
    struct ipv4_addr dst_ip_mask;
    uint8_t  action;        /* fw_action_t */
    uint8_t  log;
    uint16_t _pad;
    uint32_t hit_count;
    uint32_t id;            /* assigned at add time */
    char     name[32];
} firewall_rule_t;

/* ── Connection track entry ──────────────────────────────────────── */

typedef struct {
    uint8_t          in_use;
    uint8_t          proto;        /* IP_PROTO_* */
    uint8_t          state;        /* fw_conn_state_t */
    uint8_t          _pad;
    struct ipv4_addr src_ip;
    struct ipv4_addr dst_ip;
    uint16_t         src_port;
    uint16_t         dst_port;
    uint64_t         last_seen_tsc;
} firewall_conn_t;

/* ── Drop log entry ──────────────────────────────────────────────── */

typedef struct {
    uint64_t         tsc;
    struct ipv4_addr src_ip;
    struct ipv4_addr dst_ip;
    uint16_t         src_port;
    uint16_t         dst_port;
    uint8_t          direction;
    uint8_t          proto;
    uint8_t          action;       /* DROP or REJECT */
    int32_t          rule_id;
    char             reason[32];
} firewall_drop_log_t;

/* ── Public API ──────────────────────────────────────────────────── */

/* One-time init — loads rules + conntrack from VAULT, installs default
 * rules on first boot, registers settings. Idempotent. */
void firewall_init(void);

/* Register CHAIN_FIREWALL (parent CHAIN_CPU, MASQ_INTERNAL). Idempotent.
 * Returns chain id or -1 on failure. */
int  firewall_chain_register(int parent_chain_id);

/* Net-chain hook resolves: typed input/output is ethernet_frame. These
 * parse the eth/IP/TCP/UDP headers, build a firewall_check_t, fire
 * CHAIN_FIREWALL, and on DROP/REJECT mark the eth_frame as error so the
 * downstream nodes truncate and the chain emits a "dropped" completion. */
void firewall_check_tx_resolve(void *self, void *input, void *output);
void firewall_check_rx_resolve(void *self, void *input, void *output);

/* Chain id (set by firewall_chain_register, -1 before that). */
extern int CHAIN_FIREWALL;

/* Globals readable by selftest / shell. */
int      firewall_enabled(void);
void     firewall_set_enabled(int on);
int      firewall_rule_count(void);
int      firewall_active_conn_count(void);
uint32_t firewall_drop_count_total(void);
uint32_t firewall_chain_decisions_total(void);

/* Compute drops/min over the last 60s (rolling). */
uint32_t firewall_drops_per_min(void);

/* Rule mgmt — return 0 on success, -1 on full / dup, -2 on bad input. */
int firewall_add_rule(const firewall_rule_t *rule);
int firewall_del_rule(uint32_t id);
int firewall_flush_rules(void);

/* Walk rules in priority order. cb returning non-zero stops the walk. */
typedef int (*firewall_rule_walk_cb)(const firewall_rule_t *r, void *user);
int firewall_walk_rules(firewall_rule_walk_cb cb, void *user);

/* Walk conntrack. */
typedef int (*firewall_conn_walk_cb)(const firewall_conn_t *c, void *user);
int firewall_walk_conns(firewall_conn_walk_cb cb, void *user);

/* Read recent drop log. n=count, idx=0 is newest. Returns 0 on success. */
int firewall_log_count(void);
int firewall_log_get(int idx, firewall_drop_log_t *out);

/* Persist current rule table + 32 most recent conntrack entries. */
int firewall_persist_save(void);
int firewall_persist_restore(void);

/* Selftest line — "Firewall .............. enabled, N rules, M active connections, K drops/min". */
void firewall_print_selftest_line(void);

/* Settings registry hook — called from settings_register_all. */
void firewall_settings_register(void);

/* Shell command (handles all subcommands: list, add, del, flush, log,
 * enable, disable). */
void firewall_cmd(const char *args);

#endif /* ZEOS_FIREWALL_H */
