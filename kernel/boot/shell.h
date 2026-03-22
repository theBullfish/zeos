/*
 * Zeos — Minimal shell
 *
 * First interactive interface. Type commands, get responses.
 * Proof that Zeos can talk to a human.
 */

#ifndef ZEOS_SHELL_H
#define ZEOS_SHELL_H

#include "zeos_boot.h"

/* Run the shell (never returns) */
void shell_run(struct zeos_boot_info *boot);

#endif /* ZEOS_SHELL_H */
