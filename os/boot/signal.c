/*
 * Zeos — Signal Chain Engine
 *
 * Dataflow graph execution. Nodes fire when inputs are satisfied.
 * Data propagates along edges. The graph resolves in waves until
 * no more nodes can fire.
 *
 * This is not a scheduler in the traditional sense. There are no
 * time slices, no preemption, no priority levels. A chain resolves
 * completely or blocks waiting for external input. The graph
 * determines execution order, not the OS.
 *
 * Timing is built in: every node records TSC at start and end.
 * This is Zixel — computation IS telemetry.
 */

#include "signal.h"
#include "fb.h"
#include "timer.h"   /* timer_read_tsc() — portable cycle counter */

static struct sig_chain chains[SIG_MAX_CHAINS];
static int chain_count;

/* Read the cycle/tick counter (arch-portable). */
/* Cycle counter via the shared timer contract. This used to be an
 * #if defined(__aarch64__) picking between rdtsc and cntpct_el0 — an arch
 * conditional inside the chip-agnostic layer, which means every new arch edits
 * this file. timer_read_tsc() is stateless on both arches and each installer
 * supplies its own, so riscv64 will need no change here. */
static inline uint64_t read_tsc(void) { return timer_read_tsc(); }

/* Copy signal data */
static void sig_data_copy(struct sig_data *dst, struct sig_data *src)
{
    dst->size = src->size;
    dst->type = src->type;
    for (uint32_t i = 0; i < src->size && i < SIG_BUFFER_SIZE; i++)
        dst->data[i] = src->data[i];
}

void sig_init(void)
{
    chain_count = 0;
    for (int i = 0; i < SIG_MAX_CHAINS; i++) {
        chains[i].active = 0;
        chains[i].node_count = 0;
    }
}

int sig_chain_create(const char *name)
{
    if (chain_count >= SIG_MAX_CHAINS)
        return -1;

    int id = chain_count++;
    struct sig_chain *chain = &chains[id];
    chain->id = id;
    chain->name = name;
    chain->node_count = 0;
    chain->active = 1;
    chain->tsc_start = 0;
    chain->tsc_end = 0;
    chain->resolve_count = 0;

    return id;
}

int sig_node_add(int chain_id, const char *name, sig_process_fn process,
                 void *user_data)
{
    if (chain_id < 0 || chain_id >= chain_count)
        return -1;

    struct sig_chain *chain = &chains[chain_id];
    if (chain->node_count >= SIG_MAX_NODES)
        return -1;

    int idx = chain->node_count++;
    struct sig_node *node = &chain->nodes[idx];
    node->id = idx;
    node->name = name;
    node->state = SIG_IDLE;
    node->process = process;
    node->user_data = user_data;
    node->output_count = 0;
    node->input_required = 0;
    node->inputs_received = 0;
    node->input.size = 0;
    node->output.size = 0;
    node->tsc_start = 0;
    node->tsc_end = 0;

    return idx;
}

int sig_edge_add(int chain_id, int src_idx, int dst_idx)
{
    if (chain_id < 0 || chain_id >= chain_count)
        return -1;

    struct sig_chain *chain = &chains[chain_id];
    if (src_idx < 0 || src_idx >= chain->node_count)
        return -1;
    if (dst_idx < 0 || dst_idx >= chain->node_count)
        return -1;

    struct sig_node *src = &chain->nodes[src_idx];
    if (src->output_count >= SIG_MAX_EDGES)
        return -1;

    src->output_nodes[src->output_count++] = dst_idx;

    /* The destination needs one more input satisfied before it can fire */
    chain->nodes[dst_idx].input_required++;

    return 0;
}

int sig_inject(int chain_id, int node_idx, struct sig_data *data)
{
    if (chain_id < 0 || chain_id >= chain_count)
        return -1;

    struct sig_chain *chain = &chains[chain_id];
    if (node_idx < 0 || node_idx >= chain->node_count)
        return -1;

    struct sig_node *node = &chain->nodes[node_idx];
    sig_data_copy(&node->input, data);
    node->inputs_received = node->input_required;  /* Satisfy all inputs */
    node->state = SIG_READY;

    return 0;
}

int sig_resolve(int chain_id)
{
    if (chain_id < 0 || chain_id >= chain_count)
        return 0;

    struct sig_chain *chain = &chains[chain_id];
    chain->tsc_start = read_tsc();

    int total_fired = 0;
    int fired;

    /* Keep resolving until no more nodes fire */
    do {
        fired = 0;

        for (int i = 0; i < chain->node_count; i++) {
            struct sig_node *node = &chain->nodes[i];

            /* Skip nodes that aren't ready */
            if (node->state != SIG_READY)
                continue;

            /* Fire this node */
            node->state = SIG_RUNNING;
            node->tsc_start = read_tsc();

            int result = 0;
            if (node->process) {
                result = node->process(node, &node->input, &node->output);
            }

            node->tsc_end = read_tsc();

            if (result != 0) {
                node->state = SIG_ERROR;
                continue;
            }

            node->state = SIG_DONE;
            fired++;
            total_fired++;

            /* Propagate output to downstream nodes */
            for (int e = 0; e < node->output_count; e++) {
                int dst_idx = node->output_nodes[e];
                struct sig_node *dst = &chain->nodes[dst_idx];

                /* Copy output → downstream input */
                sig_data_copy(&dst->input, &node->output);
                dst->inputs_received++;

                /* Check if downstream node is now ready */
                if (dst->inputs_received >= dst->input_required) {
                    dst->state = SIG_READY;
                }
            }
        }
    } while (fired > 0);

    chain->tsc_end = read_tsc();
    chain->resolve_count++;

    return total_fired;
}

struct sig_chain *sig_get_chain(int chain_id)
{
    if (chain_id < 0 || chain_id >= chain_count)
        return 0;
    return &chains[chain_id];
}

int sig_chain_count(void)
{
    return chain_count;
}
