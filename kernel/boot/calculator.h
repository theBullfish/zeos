/*
 * Zeos — Calculator
 *
 * Standalone three-mode calculator window:
 *   STANDARD    — +, -, *, /, =, parens, sqrt, %.
 *   SCIENTIFIC  — sin, cos, tan, log, ln, exp, x^y, factorial.
 *   PROGRAMMER  — hex/dec/oct/bin, AND/OR/XOR/NOT, <<, >>, 64 bit toggles.
 *
 * Recursive-descent expression engine, history panel (last 8 evaluations),
 * keyboard + mouse input, Ctrl+1/2/3 mode switch.
 *
 * Lifecycle:
 *   calculator_open() — show the window.
 *   calculator_close() — close, prompts via ui_dirty if there's unsaved input.
 *
 * Drawing dispatched from compositor overlay layer; input dispatched from
 * mouse/keyboard. Singleton — one calculator at a time.
 */

#ifndef ZEOS_CALCULATOR_H
#define ZEOS_CALCULATOR_H

#include <stdint.h>

typedef enum {
    CALC_STANDARD = 0,
    CALC_SCIENTIFIC = 1,
    CALC_PROGRAMMER = 2,
} calc_mode_t;

/* Lifecycle. */
void calculator_open(void);
void calculator_close(void);
int  calculator_active(void);

/* Mode. */
void        calculator_set_mode(calc_mode_t mode);
calc_mode_t calculator_get_mode(void);

/* Pure evaluator (no UI). Returns 1 on success, 0 on parse/eval error.
 * `out` receives the result; `err` receives a short message on failure
 * (may be NULL). `mode` controls programmer-mode integer semantics
 * (& | ^ ~ << >>) — pass CALC_STANDARD for plain math. */
int calculator_eval(const char *expr, calc_mode_t mode,
                    double *out, char *err, int err_max);

/* Input dispatch — return 1 if consumed. */
int calculator_key(int ascii);
int calculator_key_arrow(int dir);             /* 0=L 1=R 2=U 3=D */
int calculator_mouse_down(int x, int y, int button);
int calculator_mouse_up(int x, int y, int button);
int calculator_mouse_move(int x, int y);

/* Draw — call from compositor overlay layer. */
void calculator_draw(void);

/* Counters / selftest. */
uint32_t calculator_total_opens(void);
uint32_t calculator_total_evals_ok(void);
uint32_t calculator_total_evals_fail(void);

#endif /* ZEOS_CALCULATOR_H */
