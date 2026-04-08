/*
 * Zeos — Cursor & Click Animation System
 *
 * Persona-aware cursors with spring-animated click feedback.
 * Each cursor state is a bitmap sprite. Click animations are
 * driven by the existing spring physics — no keyframes.
 *
 * Click feedback pipeline:
 *   1. Mouse down  → scale spring (1.0 → 0.85), swap to pressed sprite
 *   2. Mouse up    → scale spring (0.85 → 1.1 → 1.0), ripple ring fires
 *   3. Ripple      → radius spring (0 → 24), opacity spring (180 → 0)
 *
 * All animations use SPRING_INTERACTIVE (400/28) for snappy response.
 * Ripple uses SPRING_SMOOTH (100/16) for gentle fade.
 */

#ifndef ZEOS_CURSOR_H
#define ZEOS_CURSOR_H

#include <stdint.h>

/* ── Cursor states (maps to SVG set) ── */
typedef enum {
    CURSOR_DEFAULT,          /* 032 — clean arrow */
    CURSOR_CLICK,            /* 016 — arrow + target rings */
    CURSOR_CLICK_BURST,      /* 013 — arrow + sparkle burst */
    CURSOR_CLICK_CONFIRM,    /* 009 — arrow + checkmark */
    CURSOR_POINTER,          /* 050 — hand */
    CURSOR_TEXT,             /* 036 — I-beam */
    CURSOR_CROSSHAIR,       /* 047 — precision crosshair */
    CURSOR_MOVE,             /* 048 — d-pad circle */
    CURSOR_RESIZE_DIAG,      /* 042 — diagonal resize */
    CURSOR_GRAB,             /* 050 — open hand */
    CURSOR_ADD,              /* 006 — arrow + plus */
    CURSOR_REMOVE,           /* 007 — arrow + minus */
    CURSOR_DELETE,           /* 008 — arrow + X */
    CURSOR_FORBIDDEN,        /* 020 — arrow + no-entry */
    CURSOR_HELP,             /* 015 — arrow + question */
    CURSOR_BUSY,             /* 025 — arrow + clock */
    CURSOR_WAIT,             /* 040 — hourglass */
    CURSOR_LOADING,          /* 011 — arrow + spinner */
    CURSOR_PROGRESS,         /* 003 — arrow + refresh */
    CURSOR_SELECT,           /* 012 — arrow + selection box */
    CURSOR_ALERT,            /* 014 — arrow + alert */
    CURSOR_DRAG,             /* 004 — arrow + 4-way */
    CURSOR_COUNT
} cursor_state_t;

/* ── Click animation types ── */
typedef enum {
    CLICK_ANIM_NONE,         /* no feedback */
    CLICK_ANIM_SCALE,        /* scale pulse (default) */
    CLICK_ANIM_RIPPLE,       /* expanding ring at click point */
    CLICK_ANIM_BURST,        /* swap to burst sprite briefly */
    CLICK_ANIM_CONFIRM,      /* swap to checkmark on success */
} click_anim_type_t;

/* ── Ripple effect state ── */
typedef struct {
    int x, y;                /* click origin */
    float radius;            /* current radius (spring-driven) */
    float opacity;           /* current opacity (spring-driven) */
    int anim_radius;         /* spring anim id for radius */
    int anim_opacity;        /* spring anim id for opacity */
    uint32_t color;          /* persona accent */
    uint8_t active;
} click_ripple_t;

/* ── Cursor system state ── */
typedef struct {
    /* Position */
    int x, y;

    /* Current visual state */
    cursor_state_t state;
    cursor_state_t prev_state;      /* for returning after click anim */

    /* Click animation */
    click_anim_type_t click_anim;
    float scale;                     /* spring-driven, 1.0 = normal */
    int anim_scale;                  /* spring anim id */
    uint8_t pressed;                 /* mouse button held */

    /* Ripple pool */
    #define MAX_RIPPLES 4
    click_ripple_t ripples[MAX_RIPPLES];

    /* Persona accent (set on persona switch) */
    uint32_t accent;
    uint32_t accent_dim;
} cursor_t;

/* ── API ── */

/* Initialize cursor system */
void cursor_init(void);

/* Set cursor state (e.g. CURSOR_TEXT when over editable area) */
void cursor_set(cursor_state_t state);

/* Update cursor position */
void cursor_move(int x, int y);

/* Mouse button events — triggers click animations */
void cursor_press(void);
void cursor_release(void);

/* Success feedback — briefly shows checkmark */
void cursor_confirm(void);

/* Set click animation style */
void cursor_set_click_anim(click_anim_type_t type);

/* Set persona colors (called on persona switch) */
void cursor_set_persona(uint32_t accent, uint32_t accent_dim);

/* Select cursor colorway by persona index (0=zeros, 1=derez, 2=full) */
void cursor_select_colorway(int index);
int  cursor_get_colorway(void);
void cursor_cycle_colorway(void);

/* Tick — call every frame, drives spring animations */
void cursor_tick(float dt);

/* Draw — renders cursor + any active click effects */
void cursor_draw(void);

/* ── Click animation spring presets ── */
/* Scale pulse: snappy, no bounce */
#define CLICK_SCALE_STIFFNESS   400.0f
#define CLICK_SCALE_DAMPING      28.0f

/* Ripple expand: smooth outward */
#define RIPPLE_EXPAND_STIFFNESS  100.0f
#define RIPPLE_EXPAND_DAMPING     16.0f

/* Ripple fade: smooth disappear */
#define RIPPLE_FADE_STIFFNESS     80.0f
#define RIPPLE_FADE_DAMPING       14.0f

/* Burst sprite hold time (frames at 60fps) */
#define BURST_HOLD_FRAMES         8

/* Ripple max radius (pixels) */
#define RIPPLE_MAX_RADIUS         24

/* Scale targets */
#define SCALE_NORMAL              1.0f
#define SCALE_PRESSED             0.85f
#define SCALE_RELEASE_OVERSHOOT   1.1f

#endif /* ZEOS_CURSOR_H */
