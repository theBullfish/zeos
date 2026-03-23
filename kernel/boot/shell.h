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

/* Run the shell (never returns) */
void shell_run(struct zeos_boot_info *boot);

#endif /* ZEOS_SHELL_H */
