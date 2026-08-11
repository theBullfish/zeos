/*
 * Zeos -- First-boot welcome flow.
 *
 * Detects per-identity-context first boot via vault key
 *   /welcome/<ctx_id>/seen      uint32 flag (0 or 1)
 *   /welcome/<ctx_id>/seeded    uint32 flag (0 or 1)
 *
 * On first boot for a given ctx_id (after cold-boot enrollment, before
 * the shell becomes interactive):
 *   1. Run a synchronous full-screen welcome modal that lets the user
 *      pick a persona (Zeros / DereZ / Full).
 *   2. Open a "Welcome" wm window that the user dismisses with Enter
 *      to continue into the shell.
 *   3. Pre-populate kv-zeos / notes-zeos / chat-zeos / web-zeos /
 *      build-zeos with demo content.
 *
 * Each of those steps writes its own vault marker so it doesn't run a
 * second time for the same context.
 *
 * Codex Labs LLC -- 2026
 */

#ifndef ZEOS_WELCOME_H
#define ZEOS_WELCOME_H

#include <stdint.h>

/* Returns 1 if the welcome modal has not yet run for this ctx, 0 otherwise. */
int  welcome_should_run(int ctx_id);

/* Returns 1 if demo content has not yet been seeded for this ctx, 0 otherwise. */
int  welcome_should_seed(int ctx_id);

/* Run the full-screen welcome modal. Synchronous; pumps keyboard +
 * serial. Returns the chosen persona (0=Zeros, 1=DereZ, 2=Full). On
 * exit writes /welcome/<ctx>/seen = 1. */
int  welcome_run_flow(int ctx_id);

/* Apply the chosen persona to the running shell. */
void welcome_apply_persona(int persona_idx);

/* Open the persistent "Welcome" wm window (chain-rendered, persona-
 * accented). Caller is responsible for not opening it twice. */
void welcome_open_window(int chosen_persona);

/* Seed demo content across all five replacements. Idempotent per ctx;
 * writes /welcome/<ctx>/seeded = 1 on success. */
void welcome_seed_demo_content(int ctx_id);

/* "Welcome ............... seen: yes/no, demo seeded: yes/no" */
void welcome_print_selftest_line(void);

/* Top-level entry called from main.c after identity_init + before
 * scheduler_run. Decides whether to run, runs, seeds, opens window. */
void welcome_run_if_first_boot(void);

/* Shell command handlers. */
void cmd_tour(const char *args);

#endif /* ZEOS_WELCOME_H */
