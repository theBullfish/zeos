/*
 * Zeos -- Z+ Runtime Chain Registry (implementation)
 *
 * See zp_runtime.h for the contract. This file owns the persistent
 * storage that backs every runtime Z+ chain in the kernel chain registry,
 * plus the single dispatch thunk that chain_resolve() invokes per node.
 *
 * The thunk reads node->state (set during chain_add_node) which points at
 * a struct zp_runtime_node living in this file's static `slots[]` table.
 * It walks the verb type and produces an int32 output -- the canonical
 * Z+ wire format, matching what the in-process interpreter produces in
 * zplus.c.
 */

#include "zp_runtime.h"
#include "chain.h"
#include "chain_registry.h"
#include "hda.h"
#include "net_chain.h"
#include "block_chain.h"
#include "vault.h"
#include "kprint.h"

/* ── Persistent storage ───────────────────────────────────────────── */

#define ZP_RT_MAX_CHAINS  MAX_CHAINS
#define ZP_RT_MAX_NODES   MAX_CHAIN_NODES

struct zp_runtime_chain {
    char    name[64];
    int     chain_id;       /* >=0 if currently registered, -1 if free */
    int     node_count;
    struct zp_runtime_node nodes[ZP_RT_MAX_NODES];
};

static struct zp_runtime_chain slots[ZP_RT_MAX_CHAINS];
static int slots_initialized = 0;

/* ── String helpers ───────────────────────────────────────────────── */

static int rt_streq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

static void rt_strcpy(char *dst, const char *src, int max)
{
    int i;
    for (i = 0; i < max - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* ── Type inference ───────────────────────────────────────────────── */

const char *zp_runtime_input_type(enum zp_node_type t)
{
    switch (t) {
    case ZP_EMIT:        return "trigger";
    case ZP_AUDIO_PLAY:  return "pcm_request";
    case ZP_NET_SEND:    return "frame";
    case ZP_NET_RECV:    return "trigger";
    case ZP_FS_READ:     return "block_request";
    case ZP_FS_WRITE:    return "block_request";
    case ZP_VAULT_PUT:   return "int32";
    case ZP_VAULT_GET:   return "trigger";
    case ZP_TAP_LOG:     return "int32";
    case ZP_PRINT:       return "int32";
    default:             return "int32";
    }
}

const char *zp_runtime_output_type(enum zp_node_type t)
{
    switch (t) {
    case ZP_EMIT:        return "int32";
    case ZP_AUDIO_PLAY:  return "tx_completion";
    case ZP_NET_SEND:    return "tx_completion";
    case ZP_NET_RECV:    return "frame_len";
    case ZP_FS_READ:     return "bytes_moved";
    case ZP_FS_WRITE:    return "bytes_moved";
    case ZP_VAULT_PUT:   return "int32";
    case ZP_VAULT_GET:   return "int32";
    case ZP_TAP_LOG:     return "int32";
    case ZP_PRINT:       return "int32";
    case ZP_KNEE:        return "int32";
    case ZP_SUSTAINED:   return "int32";
    case ZP_DELTA:       return "int32";
    default:             return "int32";
    }
}

/* ── Wire helpers (match zplus.c little-endian int32 framing) ─────── */

static int32_t rt_read_i32(const void *buf)
{
    const uint8_t *b = (const uint8_t *)buf;
    return (int32_t)((uint32_t)b[0] |
                     ((uint32_t)b[1] << 8) |
                     ((uint32_t)b[2] << 16) |
                     ((uint32_t)b[3] << 24));
}

static void rt_write_i32(void *buf, int32_t v)
{
    uint8_t *b = (uint8_t *)buf;
    b[0] = (uint8_t)(v & 0xFF);
    b[1] = (uint8_t)((v >> 8) & 0xFF);
    b[2] = (uint8_t)((v >> 16) & 0xFF);
    b[3] = (uint8_t)((v >> 24) & 0xFF);
}

/* ── Sustained comparator ─────────────────────────────────────────── */

static int rt_sustained_cmp(int32_t v, int32_t t, int32_t gt)
{
    switch ((enum zp_node_type)gt) {
    case ZP_GATE_GT:  return v >  t;
    case ZP_GATE_LT:  return v <  t;
    case ZP_GATE_EQ:  return v == t;
    case ZP_GATE_GTE: return v >= t;
    case ZP_GATE_LTE: return v <= t;
    default:          return v >  t;
    }
}

/* ── The dispatch thunk ───────────────────────────────────────────── */

void zp_kernel_resolve_thunk(chain_node_t *self, void *input, void *output);

void zp_kernel_resolve_thunk(chain_node_t *self, void *input, void *output)
{
    struct zp_runtime_node *n = (struct zp_runtime_node *)self->state;
    if (!n) {
        /* No AST -- treat as passthrough int32. */
        rt_write_i32(output, rt_read_i32(input));
        return;
    }

    int32_t in_val = rt_read_i32(input);
    int32_t out_val = 0;

    switch (n->type) {
    case ZP_EMIT:
        out_val = n->int_val;
        break;
    case ZP_MULTIPLY:
        out_val = in_val * n->int_val;
        break;
    case ZP_ADD:
        out_val = in_val + n->int_val;
        break;
    case ZP_SUBTRACT:
        out_val = in_val - n->int_val;
        break;
    case ZP_NEGATE:
        out_val = -in_val;
        break;
    case ZP_PASSTHROUGH:
        out_val = in_val;
        break;
    case ZP_GATE_GT:  out_val = (in_val >  n->int_val) ? in_val : 0; break;
    case ZP_GATE_LT:  out_val = (in_val <  n->int_val) ? in_val : 0; break;
    case ZP_GATE_EQ:  out_val = (in_val == n->int_val) ? in_val : 0; break;
    case ZP_GATE_GTE: out_val = (in_val >= n->int_val) ? in_val : 0; break;
    case ZP_GATE_LTE: out_val = (in_val <= n->int_val) ? in_val : 0; break;
    case ZP_KNEE: {
        int32_t lo = n->int_val, hi = n->int_val2;
        if (in_val <= lo)       out_val = 0;
        else if (in_val >= hi)  out_val = 100;
        else if (hi == lo)      out_val = 100;
        else                    out_val = ((in_val - lo) * 100) / (hi - lo);
        break;
    }
    case ZP_DELTA:
        out_val = n->has_delta_prev ? (in_val - n->delta_prev) : 0;
        n->delta_prev = in_val;
        n->has_delta_prev = 1;
        break;
    case ZP_SUSTAINED:
        if (rt_sustained_cmp(in_val, n->int_val, n->int_val3))
            n->sustain_count++;
        else
            n->sustain_count = 0;
        out_val = (n->sustain_count >= n->int_val2) ? in_val : 0;
        break;
    case ZP_AUDIO_PLAY: {
        static int16_t blip[400];
        static int initialized = 0;
        if (!initialized) {
            for (int i = 0; i < 400; i++)
                blip[i] = (i & 0x10) ? 12000 : -12000;
            initialized = 1;
        }
        int rc = hda_play_pcm(blip, 400, 8000);
        out_val = (rc == 0) ? 1 : 0;
        break;
    }
    case ZP_NET_SEND: {
        uint8_t frame[64];
        for (int i = 0; i < 6; i++)  frame[i] = 0xff;
        for (int i = 6; i < 12; i++) frame[i] = 0x00;
        frame[12] = 0x88; frame[13] = 0xB5;
        for (int i = 14; i < 64; i++) frame[i] = (uint8_t)i;
        int rc = net_chain_send(frame, sizeof(frame));
        out_val = (rc == 0) ? 1 : 0;
        break;
    }
    case ZP_NET_RECV: {
        uint8_t buf[1600];
        int len = net_chain_recv(buf, sizeof(buf));
        out_val = (len > 0) ? len : 0;
        break;
    }
    case ZP_FS_READ: {
        static uint8_t scratch[4096];
        int32_t drive = n->int_val;
        int32_t lba   = n->int_val2;
        int32_t count = n->int_val3;
        if (count > 8) count = 8;
        if (count <= 0) count = 1;
        int rc = block_chain_submit(drive, (uint64_t)lba, (uint32_t)count,
                                    BLOCK_OP_READ, scratch);
        out_val = (rc == 0) ? count * 512 : 0;
        break;
    }
    case ZP_FS_WRITE: {
        static uint8_t scratch[4096];
        int32_t drive = n->int_val;
        int32_t lba   = n->int_val2;
        int32_t count = n->int_val3;
        if (count > 8) count = 8;
        if (count <= 0) count = 1;
        int rc = block_chain_submit(drive, (uint64_t)lba, (uint32_t)count,
                                    BLOCK_OP_WRITE, scratch);
        out_val = (rc == 0) ? count * 512 : 0;
        break;
    }
    case ZP_VAULT_PUT: {
        const char *key = n->fmt;
        if (key && *key) {
            int32_t v = in_val;
            int rc = vault_save_config(key, &v, sizeof(v));
            out_val = (rc > 0) ? v : 0;
        }
        break;
    }
    case ZP_VAULT_GET: {
        const char *key = n->fmt;
        int32_t v = 0;
        if (key && *key) vault_load_config(key, &v, sizeof(v));
        out_val = v;
        break;
    }
    case ZP_TAP_LOG:
        kputs("    [tap.log ");
        kputs(n->name);
        kputs("] ");
        kput_dec((uint64_t)(uint32_t)in_val);
        kputs("\n");
        out_val = in_val;
        break;
    case ZP_PRINT: {
        const char *fmt = n->fmt;
        kputs("  ");
        if (!*fmt) {
            kput_dec((uint64_t)(uint32_t)in_val);
        } else {
            while (*fmt) {
                if (fmt[0] == '{' && fmt[1] == 'v' && fmt[2] == 'a' &&
                    fmt[3] == 'l' && fmt[4] == 'u' && fmt[5] == 'e' &&
                    fmt[6] == '}') {
                    kput_dec((uint64_t)(uint32_t)in_val);
                    fmt += 7;
                    continue;
                }
                kputc(*fmt);
                fmt++;
            }
        }
        kputs("\n");
        out_val = in_val;
        break;
    }
    case ZP_COMPUTE_RUN:
        /* compute.run is interpreter-local (needs Z+ program context).
         * In runtime chains it falls back to passthrough. */
        out_val = in_val;
        break;

    /* Pass-1: in the runtime chain, string/struct verbs are best-effort.
     * The in-process interpreter (zp_proc_*) is the canonical path; here
     * we honor what the runtime can without re-implementing pools state. */
    case ZP_STR_LEN: {
        int len = 0;
        const char *s = zp_str_get(in_val, &len);
        out_val = s ? len : 0;
        break;
    }
    case ZP_STR_LEN_OF_ARRAY:
        out_val = zp_array_len(in_val);
        break;
    case ZP_STR_UPPER: case ZP_STR_LOWER: case ZP_STR_TRIM:
    case ZP_STR_FROM_INT: case ZP_STR_TO_INT:
    case ZP_STR_FIND: case ZP_STR_SPLIT: case ZP_STR_REPLACE:
    case ZP_STR_CONCAT: case ZP_STR_STARTS_WITH: case ZP_STR_ENDS_WITH:
    case ZP_STRUCT_LITERAL: case ZP_FIELD_ACCESS:
    case ZP_GATE_FIELD_EQ:
        /* Runtime path is passthrough — interpreter owns the heavy lift. */
        out_val = (n->type == ZP_STR_TO_INT) ? 0 :
                  (n->emit_handle >= 0 ? n->emit_handle : in_val);
        break;
    case ZP_SUM:
        n->sum_acc += in_val;
        out_val = n->sum_acc;
        break;

    /* Pass 3: stdlib verbs in runtime path are passthrough.  The
     * canonical execution path is the in-process interpreter
     * (zp_proc_*).  Runtime chains that need stdlib verbs go through
     * a shim chain that re-enters zp_run.  Documented gap. */
    case ZP_TIME_NOW: case ZP_TIME_NOW_MS: case ZP_TIME_SLEEP:
    case ZP_TIME_FORMAT: case ZP_CRYPTO_HMAC: case ZP_CRYPTO_AES_ENC:
    case ZP_CRYPTO_AES_DEC: case ZP_CRYPTO_RANDOM: case ZP_JSON_PARSE:
    case ZP_JSON_EMIT: case ZP_REGEX_MATCH: case ZP_REGEX_FIND:
    case ZP_REGEX_REPLACE: case ZP_CMD_RUN: case ZP_CMD_CAPTURE:
    case ZP_HTTP_LISTEN: case ZP_HTTP_RESPOND: case ZP_HTTP_ROUTE:
    case ZP_BTREE_NEW: case ZP_BTREE_INSERT: case ZP_BTREE_GET:
    case ZP_BTREE_DELETE: case ZP_BTREE_RANGE: case ZP_FS_LIST:
    case ZP_FS_EXISTS: case ZP_FS_SIZE: case ZP_FS_READ_STRING:
    case ZP_FS_WRITE_STRING:
        out_val = in_val;
        break;
    }

    rt_write_i32(output, out_val);
}

/* ── Init / lookup ────────────────────────────────────────────────── */

void zp_runtime_init(void)
{
    if (slots_initialized) return;
    for (int i = 0; i < ZP_RT_MAX_CHAINS; i++) {
        slots[i].chain_id = -1;
        slots[i].name[0] = '\0';
        slots[i].node_count = 0;
    }
    slots_initialized = 1;
}

static struct zp_runtime_chain *rt_find_by_name(const char *name)
{
    for (int i = 0; i < ZP_RT_MAX_CHAINS; i++) {
        if (slots[i].chain_id < 0) continue;
        if (rt_streq(slots[i].name, name))
            return &slots[i];
    }
    return (struct zp_runtime_chain *)0;
}

static struct zp_runtime_chain *rt_alloc_slot(void)
{
    for (int i = 0; i < ZP_RT_MAX_CHAINS; i++) {
        if (slots[i].chain_id < 0)
            return &slots[i];
    }
    return (struct zp_runtime_chain *)0;
}

int zp_runtime_count(void)
{
    if (!slots_initialized) return 0;
    int n = 0;
    for (int i = 0; i < ZP_RT_MAX_CHAINS; i++) {
        if (slots[i].chain_id < 0) continue;
        chain_t *c = chain_get(slots[i].chain_id);
        if (c) n++;
    }
    return n;
}

int zp_runtime_register_chain(const char *name,
                              const struct zp_runtime_node *nodes,
                              int node_count)
{
    if (!slots_initialized) zp_runtime_init();
    if (!name || !*name || node_count <= 0) return -1;
    if (node_count > ZP_RT_MAX_NODES) node_count = ZP_RT_MAX_NODES;

    /* Lifetime: tear down any prior runtime chain with the same name. */
    struct zp_runtime_chain *slot = rt_find_by_name(name);
    if (slot) {
        chain_destroy(slot->chain_id);
        slot->chain_id = -1;
        slot->node_count = 0;
        slot->name[0] = '\0';
    }

    /* Allocate a fresh slot. */
    slot = rt_alloc_slot();
    if (!slot) return -1;

    /* Copy node ASTs into persistent storage BEFORE chain_add_node so the
     * state pointer we hand the chain registry stays valid. */
    rt_strcpy(slot->name, name, sizeof(slot->name));
    slot->node_count = node_count;
    for (int i = 0; i < node_count; i++) {
        slot->nodes[i] = nodes[i];
        slot->nodes[i].sustain_count = 0;
        slot->nodes[i].delta_prev = 0;
        slot->nodes[i].has_delta_prev = 0;
        slot->nodes[i].sum_acc = 0;
    }

    /* Create the live chain. CHAIN_CPU as parent so it sits under the
     * compute spine, MASQ_INTERNAL so it's visible to the sovereign
     * shell but not exported as reference. */
    int parent = (CHAIN_CPU >= 0) ? CHAIN_CPU : -1;
    int cid = chain_create(name, parent, MASQ_INTERNAL);
    if (cid < 0) {
        slot->name[0] = '\0';
        slot->node_count = 0;
        return -1;
    }
    slot->chain_id = cid;

    /* Add nodes with the dispatch thunk; pass the runtime node as state. */
    for (int i = 0; i < node_count; i++) {
        const char *itype = zp_runtime_input_type(slot->nodes[i].type);
        const char *otype = zp_runtime_output_type(slot->nodes[i].type);
        int nid = chain_add_node(cid, slot->nodes[i].name,
                                 itype, otype,
                                 zp_kernel_resolve_thunk);
        if (nid < 0) {
            chain_destroy(cid);
            slot->chain_id = -1;
            slot->name[0] = '\0';
            slot->node_count = 0;
            return -1;
        }
        /* Stash AST pointer on the node for the thunk. */
        chain_t *c = chain_get(cid);
        if (c) c->nodes[nid].state = &slot->nodes[i];
    }

    return cid;
}
