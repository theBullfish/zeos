/*
 * Zeos — Signal Chain Primitive
 *
 * The fundamental execution unit. Not a thread. Not a process.
 * A signal chain is a directed graph of nodes, each with an
 * input buffer and a processing function. Data flows along edges.
 *
 * The graph resolver executes chains: when a node's inputs are
 * satisfied, it fires. This is dataflow execution, not time-slicing.
 *
 * TRISA's two-stage pipeline (detect → classify) is a signal chain.
 * Audio routing is a signal chain. A shell command is a signal chain.
 * Everything in Zeos is a signal chain.
 */

#ifndef ZEOS_SIGNAL_H
#define ZEOS_SIGNAL_H

#include <stdint.h>

/* Maximum nodes per chain, maximum chains, max edges per node */
#define SIG_MAX_NODES      64
#define SIG_MAX_CHAINS     32
#define SIG_MAX_EDGES      8
#define SIG_BUFFER_SIZE    256

/* Signal data — a typed buffer */
struct sig_data {
    uint8_t  data[SIG_BUFFER_SIZE];
    uint32_t size;       /* Bytes of valid data */
    uint32_t type;       /* Application-defined type tag */
};

/* Signal node states */
enum sig_node_state {
    SIG_IDLE,       /* Waiting for input */
    SIG_READY,      /* All inputs satisfied, ready to fire */
    SIG_RUNNING,    /* Currently executing */
    SIG_DONE,       /* Fired, output available */
    SIG_ERROR,      /* Processing failed */
};

/* Forward declarations */
struct sig_node;
struct sig_chain;

/*
 * Node processing function.
 * Receives input data, produces output data.
 * Returns 0 on success, nonzero on error.
 */
typedef int (*sig_process_fn)(struct sig_node *node,
                              struct sig_data *input,
                              struct sig_data *output);

/*
 * Signal node — one vertex in the graph.
 */
struct sig_node {
    uint32_t        id;
    const char     *name;
    enum sig_node_state state;
    sig_process_fn  process;
    void           *user_data;      /* Node-specific context */

    /* Input/output buffers */
    struct sig_data input;
    struct sig_data output;

    /* Edges: indices of downstream nodes in the chain */
    int             output_nodes[SIG_MAX_EDGES];
    int             output_count;

    /* How many inputs need to be satisfied before this node fires */
    int             input_required;
    int             inputs_received;

    /* Timing (Zixel) — TSC at fire start and end */
    uint64_t        tsc_start;
    uint64_t        tsc_end;
};

/*
 * Signal chain — a directed acyclic graph of nodes.
 */
struct sig_chain {
    uint32_t        id;
    const char     *name;
    struct sig_node nodes[SIG_MAX_NODES];
    int             node_count;
    int             active;         /* Is this chain running? */

    /* Chain-level timing */
    uint64_t        tsc_start;
    uint64_t        tsc_end;
    uint64_t        resolve_count;  /* How many times resolved */
};

/* Initialize the signal subsystem */
void sig_init(void);

/* Create a new chain. Returns chain ID, or -1 on failure. */
int sig_chain_create(const char *name);

/* Add a node to a chain. Returns node index, or -1 on failure. */
int sig_node_add(int chain_id, const char *name, sig_process_fn process, void *user_data);

/* Connect node src → node dst (within the same chain) */
int sig_edge_add(int chain_id, int src_idx, int dst_idx);

/* Inject data into a node's input (triggers the chain) */
int sig_inject(int chain_id, int node_idx, struct sig_data *data);

/*
 * Resolve a chain: fire all nodes whose inputs are satisfied,
 * propagate outputs along edges, repeat until no more nodes fire.
 * This is ONE resolution pass. Returns number of nodes fired.
 */
int sig_resolve(int chain_id);

/* Get a chain by ID */
struct sig_chain *sig_get_chain(int chain_id);

/* Get total chain count */
int sig_chain_count(void);

#endif /* ZEOS_SIGNAL_H */
