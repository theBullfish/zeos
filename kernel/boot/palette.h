/*
 * Zeos -- Command Palette
 *
 * Sacred top-left corner action. Searchable overlay for every action
 * in the OS. Activated by Super+Space.
 *
 * Substring match filter, static arrays, no malloc.
 */

#ifndef ZEOS_PALETTE_H
#define ZEOS_PALETTE_H

#include <stdint.h>
#include "keybinds.h"

#define PALETTE_MAX_ITEMS  64
#define PALETTE_MAX_QUERY 128

typedef struct {
    char label[64];
    char category[32];    /* "chain", "window", "setting", "app" */
    action_id_t action_id; /* Maps to action_id_t from keybinds.h or custom */
    void (*execute)(void *ctx);
    void *ctx;
} palette_item_t;

typedef struct {
    int              visible;
    char             query[PALETTE_MAX_QUERY];
    int              query_len;
    palette_item_t   items[PALETTE_MAX_ITEMS];
    int              item_count;
    palette_item_t   filtered[PALETTE_MAX_ITEMS];
    int              filtered_count;
    int              selected;       /* Currently highlighted item */
    int              scroll_offset;
} palette_state_t;

/* Initialize palette and register default items */
void palette_init(void);

/* Show the palette (clear query, refresh items, make visible) */
void palette_show(void);

/* Dismiss the palette */
void palette_hide(void);

/* Toggle visibility */
void palette_toggle(void);

/* Type a character into the search query */
void palette_type(char c);

/* Remove last character from query */
void palette_backspace(void);

/* Navigate selection up */
void palette_up(void);

/* Navigate selection down */
void palette_down(void);

/* Execute the currently selected item's action */
void palette_execute(void);

/* Render the palette overlay */
void palette_draw(void);

/* Register a new action item. Returns 0 on success, -1 if full. */
int palette_add_item(const char *label, const char *category,
                     void (*execute_fn)(void *ctx), void *ctx);

/* Get palette state (for input routing) */
int palette_is_visible(void);

/* Right-click on a result row — opens a context menu. */
int palette_right_click(int x, int y);

#endif /* ZEOS_PALETTE_H */
