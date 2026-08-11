/*
 * Zeos — Undo / Redo implementation
 *
 * Doctrine: every editable text widget owns a ring. The ring writes
 * forward; undo walks backward; redo walks forward again. A new edit
 * after an undo truncates the redo tail — same model every editor on
 * earth uses, because users expect it.
 */

#include "ui_undo.h"
#include "timeofday.h"

/* ── Local helpers (no libc) ── */

static int us_strlen(const char *s) {
    int n = 0; if (!s) return 0;
    while (s[n]) n++;
    return n;
}

static void us_strncpy(char *dst, const char *src, int max) {
    int i = 0;
    if (!dst || max <= 0) return;
    if (src) {
        while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    }
    dst[i] = '\0';
}

/* ── Module state ── */

static undo_buffer_t *g_focus = (void *)0;
static uint32_t g_total_records = 0;
static uint32_t g_total_undos = 0;
static uint32_t g_total_redos = 0;

/* ── API ── */

void undo_init(undo_buffer_t *buf, undo_apply_fn apply, void *ctx)
{
    if (!buf) return;
    for (int i = 0; i < UI_UNDO_RING_SIZE; i++) {
        buf->ring[i].used = 0;
        buf->ring[i].position = 0;
        buf->ring[i].deleted[0] = '\0';
        buf->ring[i].inserted[0] = '\0';
        buf->ring[i].timestamp = 0;
    }
    buf->head = 0;
    buf->count = 0;
    buf->cursor = 0;
    buf->apply = apply;
    buf->ctx = ctx;
    buf->in_transaction = 0;
}

void undo_begin(undo_buffer_t *buf)
{
    if (!buf) return;
    /* Truncate redo tail — any records past cursor are abandoned. */
    while (buf->count > buf->cursor) {
        int last = (buf->head - 1 + UI_UNDO_RING_SIZE) % UI_UNDO_RING_SIZE;
        buf->ring[last].used = 0;
        buf->head = last;
        buf->count--;
    }
}

void undo_record(undo_buffer_t *buf, int pos,
                 const char *deleted, const char *inserted)
{
    if (!buf || buf->in_transaction) return;

    /* Drop redo tail on new edit (in case caller forgot undo_begin). */
    while (buf->count > buf->cursor) {
        int last = (buf->head - 1 + UI_UNDO_RING_SIZE) % UI_UNDO_RING_SIZE;
        buf->ring[last].used = 0;
        buf->head = last;
        buf->count--;
    }

    undo_record_t *r = &buf->ring[buf->head];
    r->position = pos;
    us_strncpy(r->deleted,  deleted,  UI_UNDO_TEXT_MAX);
    us_strncpy(r->inserted, inserted, UI_UNDO_TEXT_MAX);
    r->timestamp = tod_now_unix();
    r->used = 1;

    buf->head = (buf->head + 1) % UI_UNDO_RING_SIZE;
    if (buf->count < UI_UNDO_RING_SIZE) buf->count++;
    buf->cursor = buf->count;
    g_total_records++;
}

int undo_perform(undo_buffer_t *buf)
{
    if (!buf || buf->cursor <= 0) return 0;
    int idx = (buf->head - (buf->count - buf->cursor) - 1 + UI_UNDO_RING_SIZE)
              % UI_UNDO_RING_SIZE;
    undo_record_t *r = &buf->ring[idx];
    if (!r->used) return 0;

    if (buf->apply) {
        buf->in_transaction = 1;
        int ins_len = us_strlen(r->inserted);
        int del_len = us_strlen(r->deleted);
        /* Reverse: replace inserted with deleted at position. */
        buf->apply(buf->ctx, r->position, r->inserted, ins_len,
                   r->deleted, del_len);
        buf->in_transaction = 0;
    }
    buf->cursor--;
    g_total_undos++;
    return 1;
}

int undo_redo(undo_buffer_t *buf)
{
    if (!buf || buf->cursor >= buf->count) return 0;
    int idx = (buf->head - (buf->count - buf->cursor) + UI_UNDO_RING_SIZE)
              % UI_UNDO_RING_SIZE;
    undo_record_t *r = &buf->ring[idx];
    if (!r->used) return 0;

    if (buf->apply) {
        buf->in_transaction = 1;
        int ins_len = us_strlen(r->inserted);
        int del_len = us_strlen(r->deleted);
        /* Forward: replace deleted with inserted at position. */
        buf->apply(buf->ctx, r->position, r->deleted, del_len,
                   r->inserted, ins_len);
        buf->in_transaction = 0;
    }
    buf->cursor++;
    g_total_redos++;
    return 1;
}

void undo_set_focus(undo_buffer_t *buf) { g_focus = buf; }
undo_buffer_t *undo_get_focus(void)     { return g_focus; }

int undo_handle_key(int ascii, int shift_held)
{
    if (!g_focus) return 0;
    /* Ctrl-Z arrives as ASCII 0x1A (SUB), Ctrl-Y as 0x19 (EM). */
    if (ascii == 0x1A) {
        if (shift_held) undo_redo(g_focus);
        else            undo_perform(g_focus);
        return 1;
    }
    if (ascii == 0x19) {
        undo_redo(g_focus);
        return 1;
    }
    return 0;
}

uint32_t undo_total_records(void) { return g_total_records; }
uint32_t undo_total_undos(void)   { return g_total_undos; }
uint32_t undo_total_redos(void)   { return g_total_redos; }
