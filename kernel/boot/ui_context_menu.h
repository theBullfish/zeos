/*
 * Zeos — Context Menus (right-click popups)
 *
 * Doctrine: every clickable thing should accept a right-click. The
 * menu is just a list of {label, callback, enabled}. The system owns
 * one popup at a time — opening a new one closes the previous. Esc
 * or click-outside dismisses. Hover-highlight, click invokes.
 */

#ifndef ZEOS_UI_CONTEXT_MENU_H
#define ZEOS_UI_CONTEXT_MENU_H

#include <stdint.h>

#define CTX_MENU_MAX_ITEMS   16
#define CTX_MENU_LABEL_MAX   48

typedef void (*ctx_menu_action_fn)(void *ctx);

typedef struct {
    char               label[CTX_MENU_LABEL_MAX];
    ctx_menu_action_fn action;   /* NULL = separator if label[0]=='-' */
    void              *ctx;
    int                enabled;
} ctx_menu_item_t;

/* Open a menu at screen (x,y). `items` is copied into internal storage,
 * so the caller can free / overwrite immediately. count <= CTX_MENU_MAX_ITEMS. */
void context_menu_open(int x, int y, const ctx_menu_item_t *items, int count);

/* Close the active menu (if any). */
void context_menu_close(void);

/* True if a menu is currently shown. */
int  context_menu_active(void);

/* Mouse + key dispatch. Return 1 if event was consumed. */
int  context_menu_mouse_move(int x, int y);
int  context_menu_mouse_down(int x, int y, int button);
int  context_menu_key(int ascii);

/* Draw the active menu (overlay layer). */
void context_menu_draw(void);

/* Counter for selftest. */
uint32_t context_menu_total_opens(void);

#endif /* ZEOS_UI_CONTEXT_MENU_H */
