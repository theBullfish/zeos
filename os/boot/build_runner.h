/*
 * Zeos — build-zeos rule runner.
 *
 * Real, end-to-end rule execution behind `build hello`. Rules are
 * registered at boot from a tiny seed table that mirrors the rule
 * declarations in programs/build-zeos.zp. Each rule:
 *
 *   - registers itself with build_polish (DAG window picks up name +
 *     inputs + output + command, paints state transitions live)
 *   - executes via the in-tree primitive runner — `echo X` produces X,
 *     `wc -l <vault-path>` produces the line count of that vault file
 *     — and the captured stdout is written back to the rule's output
 *     vault path
 *   - tracks elapsed milliseconds; build_polish_rule_state surfaces it
 *     in the DAG detail pane
 *
 * Honest scope:
 *   The two seeded rules ("hello", "count_chains") are the demonstration
 *   that a real DSL → executor → vault-output pipeline works end-to-end.
 *   The full bazel-class shell-out path goes through cmd.run /
 *   shell_dispatch_external, which is the same pump the rest of Zeos
 *   uses. Adding more rules is a registration call — no parser change.
 */

#ifndef ZEOS_BUILD_RUNNER_H
#define ZEOS_BUILD_RUNNER_H

#include <stdint.h>

/* One-time init. Seeds the rule table and registers each rule with
 * build_polish. Idempotent. */
void build_runner_init(void);

/* Run the named rule. Resolves dependencies first (topo order, simple
 * DFS — cycles are rejected). Returns 0 on success, -1 on missing
 * rule, -2 on dep cycle, -3 on rule failure. */
int  build_runner_run(const char *name);

/* Remove every output produced by registered rules. No-op for missing
 * outputs. Returns the number of paths cleaned. */
int  build_runner_clean(void);

/* Selftest counters. */
int      build_runner_rule_count(void);
uint32_t build_runner_last_ms(void);

#endif /* ZEOS_BUILD_RUNNER_H */
