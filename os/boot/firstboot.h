/*
 * Zeos -- First Boot Wizard
 *
 * Five screens, under 60 seconds. Every choice is visual, one click,
 * reversible later. Each screen passively teaches one Z-OS concept.
 *
 * Screen 1: Welcome        (teaches: signals exist)
 * Screen 2: Persona        (teaches: OS adapts to you)
 * Screen 3: Window Controls (teaches: detach != close)
 * Screen 4: Appearance     (teaches: everything adjustable)
 * Screen 5: Done           (teaches: two paths)
 *
 * After completion, results are stored in firstboot_config and
 * a VAULT flag prevents re-running on subsequent boots.
 */

#ifndef ZEOS_FIRSTBOOT_H
#define ZEOS_FIRSTBOOT_H

#include <stdint.h>

/* Configuration produced by the wizard */
struct firstboot_config {
    uint8_t persona;        /* 0 = Zeros, 1 = DereZ, 2 = Full */
    uint8_t controls_side;  /* 0 = left, 1 = right */
    uint8_t theme;          /* 0 = light, 1 = dark, 2 = auto */
    uint8_t density;        /* 0 = comfortable, 1 = standard, 2 = compact */
};

/* Returns 1 if first boot wizard should run (no previous completion flag) */
int firstboot_should_run(void);

/*
 * Run the first boot wizard. Blocks until the user completes all 5 screens.
 * Returns the chosen configuration. The caller should apply these to the
 * active persona, theme engine, and window manager.
 */
struct firstboot_config firstboot_run(void);

/* Mark first boot as complete (called automatically by firstboot_run) */
void firstboot_mark_complete(void);

#endif /* ZEOS_FIRSTBOOT_H */
