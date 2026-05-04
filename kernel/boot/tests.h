/*
 * Zeos test framework header.
 *
 * Per-chain tests with b3 + vault_version snapshot/restore for isolation.
 * Run via the `tests` shell command. JSON output via `tests --json`.
 */
#ifndef ZEOS_TESTS_H
#define ZEOS_TESTS_H

#include <stdint.h>

#define TEST_PASS    0
#define TEST_FAIL    1
#define TEST_SKIP    2

/*
 * Test function returns one of TEST_PASS / TEST_FAIL / TEST_SKIP.
 * The `reason` buffer (optional) may be filled with up to 63 chars
 * explaining a SKIP or FAIL; framework will print it.
 */
typedef int (*test_fn_t)(char *reason, uint32_t reason_size);

/* Register a test. chain_id is the chain whose b3+vault_version are
 * snapshotted/restored across the test. Pass -1 for cross-chain tests
 * (no snapshot). Returns 0 on success, -1 if registry full. */
int test_register(const char *name, test_fn_t fn, int chain_id);

/* Boot-time hook: register every built-in test. Called from shell init. */
void tests_register_all(void);

/* Run every registered test. Prints progress + summary.
 * Returns failure count. */
int test_run_all(void);

/* Run only tests whose name contains `substring`.
 * Returns failure count, or -1 if no tests matched. */
int test_run_named(const char *substring);

/* Same as test_run_all but emits JSON suitable for serial capture / CI. */
int test_run_all_json(void);

#endif /* ZEOS_TESTS_H */
