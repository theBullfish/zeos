/*
 * Zeos — Shell with Persona System
 *
 * Three modes, one shell:
 *   zeros>  — robotics/hardware (Zeros)
 *   derez>  — code/AI (DereZ)
 *   zeos>   — full system (curtain raised)
 *
 * Every command exists in every mode. The persona filters
 * what 'help' surfaces, not what's available.
 */

#ifndef ZEOS_SHELL_H
#define ZEOS_SHELL_H

#include "zeos_boot.h"

/* Initialize shell state (VAULT ramdisk, networking, banner). The
 * scheduler-driven main loop calls this once before entering the
 * resolve loop. Does not block. */
void shell_init(struct zeos_boot_info *boot);

/* Feed one keyboard character to the shell line editor. Returns 1 if
 * the character completed a command (the command has been dispatched
 * and a fresh prompt has been printed), 0 otherwise. Non-blocking. */
int  shell_pump_char(char c);

/* Re-print the shell prompt -- called by scheduler_init after the
 * boot banner so the user sees a prompt even before the first key. */
void shell_print_prompt(void);

/* Legacy entry point: initializes shell and hands control to the
 * scheduler. Never returns. Kept so main.c keeps calling shell_run. */
void shell_run(struct zeos_boot_info *boot);

#endif /* ZEOS_SHELL_H */
