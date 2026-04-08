/*
 * Zeos — Panel (Top Bar)
 *
 * Three-zone top bar:
 *   Left:   Persona indicator dot + name. Click = command palette.
 *   Center: Chain pills — one per visible WM surface.
 *   Right:  System health dot, notification badge, clock.
 *
 * Refreshed each frame from WM surface list + chain states.
 * Static state, bare-metal C, no allocations.
 */

#ifndef ZEOS_PANEL_H
#define ZEOS_PANEL_H

#include <stdint.h>
#include "chain.h"

#define PANEL_MAX_PILLS  16

/* ── Left zone ── */
#define PANEL_LEFT_W     120

/* ── Right zone ── */
#define PANEL_RIGHT_W    160

/* ── Pill layout ── */
#define PANEL_PILL_PAD     8   /* Horizontal spacing between pills */
#define PANEL_PILL_VPAD    4   /* Vertical inset from panel top/bottom */
#define PANEL_PILL_HPAD   12   /* Text padding inside pill */

/* ── Persona dot ── */
#define PANEL_DOT_RADIUS   5
#define PANEL_DOT_X       16   /* Center X of persona dot */

typedef struct {
    int              chain_id;
    int              surface_id;
    char             name[32];
    chain_status_t   status;     /* From chain.h */
    uint32_t         color;      /* Status color */
    int              x, w;       /* Computed position/width for hit testing */
} panel_pill_t;

typedef struct {
    int              height;         /* From density setting */
    int              visible;

    /* Left zone */
    int              persona;        /* 0=zeros, 1=derez, 2=full */
    uint32_t         persona_color;

    /* Center zone — chain pills */
    panel_pill_t     pills[PANEL_MAX_PILLS];
    int              pill_count;

    /* Right zone */
    int              chain_total;
    int              chain_live;
    int              chain_error;
    char             clock_str[16];  /* "HH:MM" */
    int              notification_count;

    /* Screen width (cached for layout) */
    int              screen_w;
} panel_state_t;

/* ── API ── */

/* Initialize panel with persona and density height */
void panel_init(int persona, int height);

/* Refresh pills from WM surfaces + chain states, update clock */
void panel_update(void);

/* Render the full panel to framebuffer */
void panel_draw(void);

/* Handle click. Returns: 0=miss, 1=left zone, 2=pill (focuses surface), 3=right zone */
int  panel_click(int x, int y);

/* Change persona accent */
void panel_set_persona(int persona);

/* Current panel height */
int  panel_get_height(void);

#endif /* ZEOS_PANEL_H */
