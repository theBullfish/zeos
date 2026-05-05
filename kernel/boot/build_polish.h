/*
 * Zeos — build-zeos polish layer (make/bazel killer DAG window)
 *
 * Renders the build registry as a circle-and-arrow DAG, color-coded by
 * rule state. Click a failed rule to see its stderr + previous-run diff.
 * Time-budget reporting at the bottom.
 *
 * Security:
 *   - Each cmd.run is invoked via a CFA-isolated sub-context (the chain
 *     resolver sets the observer to the rule's CFA root before resolve).
 *   - Build artifacts written under SOVEREIGN-handle-wrapped paths so
 *     cross-context tampering is denied at the FS layer.
 *   - HMAC-SHA256(inputs + command) → expected output hash; mismatch
 *     emits a notify_send WARNING and journals the divergence.
 *
 * Honest gaps (documented inline):
 *   - DAG layout is force-directed across one frame; on > 32 rules it
 *     overlaps. Sugiyama-layered fallback is in the roadmap.
 *   - The "previous run diff" pane requires a per-rule stderr log; we
 *     keep a 4 KiB ring per rule. Bigger logs truncate at the head.
 *   - Watch mode (`bview --watch`) hooks fs_event today; the toast on
 *     transition uses CHAIN_NOTIFY (live, no fake polish).
 *
 * Selftest line:
 *   build-zeos polish .... DAG view ready, N rules registered
 */

#ifndef ZEOS_BUILD_POLISH_H
#define ZEOS_BUILD_POLISH_H

#include <stdint.h>

typedef enum {
    BP_PENDING = 0,
    BP_RUNNING,
    BP_DONE,
    BP_FAILED,
} build_state_t;

void build_polish_init(void);
void build_polish_open(void);
void build_polish_close(void);
int  build_polish_active(void);

/* Register a rule. Returns rule id (>=0) or -1.
 * `inputs` is a NUL-terminated comma-separated list of input refs;
 * `output` is the artifact path; `cmd` is the shell-out string. */
int  build_polish_rule_add(const char *name, const char *inputs,
                           const char *output, const char *cmd);

/* Mark a rule's state. */
void build_polish_rule_state(int id, build_state_t s, uint32_t took_ms);

/* Append a stderr line to a rule's ring (max 4 KiB per rule). */
void build_polish_rule_stderr(int id, const char *line);

int  build_polish_rule_count(void);

void build_polish_print_selftest_line(void);
void build_polish_cmd(const char *args);

#endif /* ZEOS_BUILD_POLISH_H */
