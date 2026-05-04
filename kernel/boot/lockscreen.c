/*
 * Idle-driven lock screen. Input timestamps drive state transitions
 * across ACTIVE / DIMMED / LOCKED / BLANKED. PIN-only initially;
 * password / biometrics are future work.
 *
 * The PIN is stored in VAULT at /lock/pin (ASCII digits, NUL-padded).
 * On first boot a default PIN of "0000" is written so the lock command
 * has something to validate against. The default is logged loudly --
 * users are expected to `pin <new>` immediately.
 *
 * Drawing uses font_draw(FONT_BOOT, ...) so the overlay works before
 * the TTF cache is populated and during the lock-from-cold path.
 */

#include "lockscreen.h"
#include "vault.h"
#include "fb.h"
#include "font.h"
#include "kprint.h"
#include "timer.h"
#include "chain.h"
#include "chain_registry.h"

#include <stdint.h>

#define LOCK_PIN_PATH        "/lock/pin"
#define LOCK_PIN_MIN_LEN     4
#define LOCK_PIN_MAX_LEN     16
#define LOCK_DEFAULT_PIN     "0000"

#define LOCK_ENTRY_BUF       (LOCK_PIN_MAX_LEN + 1)

static char     g_stored_pin[LOCK_ENTRY_BUF];
static char     g_entry[LOCK_ENTRY_BUF];
static int      g_entry_len;
static int      g_active;
static int      g_flash_frames;          /* >0 = render red flash next draw */
static uint32_t g_failed_attempts;
static uint32_t g_unlock_count;

/* ── Local string helpers (freestanding) ──────────────────────── */

static int ls_strlen(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void ls_strncpy(char *dst, const char *src, int cap)
{
    int i = 0;
    if (cap <= 0) return;
    while (i < cap - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int ls_streq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

static int ls_is_digit(char c)
{
    return c >= '0' && c <= '9';
}

/* ── PIN persistence ──────────────────────────────────────────── */

static void lockscreen_load_pin(void)
{
    uint8_t buf[LOCK_ENTRY_BUF];
    for (int i = 0; i < LOCK_ENTRY_BUF; i++) buf[i] = 0;

    int got = vault_read(LOCK_PIN_PATH, buf, sizeof(buf) - 1);
    if (got <= 0) {
        /* First boot: seed with default. */
        ls_strncpy(g_stored_pin, LOCK_DEFAULT_PIN, LOCK_ENTRY_BUF);
        (void)vault_write(LOCK_PIN_PATH, g_stored_pin,
                          (uint32_t)ls_strlen(g_stored_pin));
        kputs("[lockscreen] WARNING: default PIN '0000' set. Run `pin <new>`.\n");
        return;
    }

    /* Sanitize: keep only leading digits up to MAX_LEN. */
    int n = 0;
    for (int i = 0; i < got && n < LOCK_PIN_MAX_LEN; i++) {
        if (ls_is_digit((char)buf[i])) g_stored_pin[n++] = (char)buf[i];
        else if (buf[i] == 0) break;
    }
    g_stored_pin[n] = '\0';
    if (n < LOCK_PIN_MIN_LEN) {
        /* Stored PIN is corrupt -- restore default. */
        ls_strncpy(g_stored_pin, LOCK_DEFAULT_PIN, LOCK_ENTRY_BUF);
        (void)vault_write(LOCK_PIN_PATH, g_stored_pin,
                          (uint32_t)ls_strlen(g_stored_pin));
    }
}

void lockscreen_init(void)
{
    g_entry_len = 0;
    g_entry[0] = '\0';
    g_active = 0;
    g_flash_frames = 0;
    g_failed_attempts = 0;
    g_unlock_count = 0;
    lockscreen_load_pin();
}

int lockscreen_pin_configured(void)
{
    return ls_strlen(g_stored_pin) >= LOCK_PIN_MIN_LEN;
}

int lockscreen_set_pin(const char *new_pin)
{
    if (!new_pin) return -1;
    int n = 0;
    while (new_pin[n] && n <= LOCK_PIN_MAX_LEN) {
        if (!ls_is_digit(new_pin[n])) return -1;
        n++;
    }
    if (n < LOCK_PIN_MIN_LEN || n > LOCK_PIN_MAX_LEN) return -1;

    ls_strncpy(g_stored_pin, new_pin, LOCK_ENTRY_BUF);
    int wrote = vault_write(LOCK_PIN_PATH, g_stored_pin, (uint32_t)n);
    if (wrote < 0) return -1;
    return 0;
}

/* ── Modal state ──────────────────────────────────────────────── */

void lockscreen_show(void)
{
    g_entry_len = 0;
    g_entry[0] = '\0';
    g_active = 1;
    g_flash_frames = 0;
}

void lockscreen_repaint(void)
{
    g_active = 1;
    g_flash_frames = 0;
}

int lockscreen_active(void) { return g_active; }
uint32_t lockscreen_failed_attempts(void) { return g_failed_attempts; }
uint32_t lockscreen_unlock_count(void)    { return g_unlock_count; }

/* ── Input ────────────────────────────────────────────────────── */

void lockscreen_input(char c)
{
    if (!g_active) return;

    if (c == '\b') {
        if (g_entry_len > 0) {
            g_entry_len--;
            g_entry[g_entry_len] = '\0';
        }
        return;
    }

    if (c == '\n' || c == '\r') {
        if (g_entry_len == 0) return;
        if (ls_streq(g_entry, g_stored_pin)) {
            /* Unlock. */
            g_unlock_count++;
            g_active = 0;
            g_entry_len = 0;
            g_entry[0] = '\0';
            g_flash_frames = 0;
            kputs("[lockscreen] unlocked\n");

            /* Bump CHAIN_IDLE vault_version on success too. */
            extern int idle_chain_id(void);
            int id = idle_chain_id();
            if (id >= 0) {
                chain_t *cc = chain_get(id);
                if (cc) cc->vault_version++;
            }
            /* Step back to ACTIVE explicitly -- idle_mark_active()
             * deliberately refuses to step out of LOCKED, so we use
             * the dedicated unlock helper. */
            extern void idle_force_unlock(void);
            idle_force_unlock();
            return;
        }

        /* Wrong PIN. Brief red flash, log, bump CHAIN_IDLE.vault_version
         * so MasQ records the failed attempt provenance. */
        g_failed_attempts++;
        g_flash_frames = 12;     /* a few hundred ms of red border */
        g_entry_len = 0;
        g_entry[0] = '\0';

        kputs("[lockscreen] failed PIN attempt #");
        kput_dec((uint64_t)g_failed_attempts);
        kputc('\n');

        extern int idle_chain_id(void);
        int id = idle_chain_id();
        if (id >= 0) {
            chain_t *cc = chain_get(id);
            if (cc) cc->vault_version++;
        }
        return;
    }

    /* Accept digits only. */
    if (!ls_is_digit(c)) return;
    if (g_entry_len >= LOCK_PIN_MAX_LEN) return;
    g_entry[g_entry_len++] = c;
    g_entry[g_entry_len] = '\0';
}

/* ── Drawing ──────────────────────────────────────────────────── */

/* Card geometry: a centered panel with the wordmark, prompt, and PIN
 * mask. We keep it deliberately spartan so this works on any
 * resolution and even with the boot bitmap font. */
#define CARD_W 480
#define CARD_H 200

void lockscreen_draw(void)
{
    if (!g_active) return;

    int sw = (int)fb_width();
    int sh = (int)fb_height();
    int cx = (sw - CARD_W) / 2;
    int cy = (sh - CARD_H) / 2;
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;

    /* Card background (dark slate, 95% opaque). */
    fb_rect_blend(cx, cy, CARD_W, CARD_H, 0xF01A1F2A);

    /* Border: red while flashing, accent otherwise. */
    uint32_t border = 0xFF3B82F6;   /* accent blue */
    if (g_flash_frames > 0) {
        border = 0xFFEF4444;        /* red */
        g_flash_frames--;
    }
    fb_rect_outline(cx, cy, CARD_W, CARD_H, border, 2);

    /* Title: "ZEOS  Locked". Boot font is 8x16 -- adequate for a modal. */
    const char *title = "ZEOS  Locked";
    int title_w = ls_strlen(title) * 8;
    int tx = cx + (CARD_W - title_w) / 2;
    int ty = cy + 32;
    font_draw(tx, ty, title, FONT_BOOT, 16, 0xFFFFFFFF);

    /* Prompt with masked entry. Spec asks for "[PIN: ____]". We use
     * '*' for entered digits, '_' for missing slots, fixed-width on
     * the boot font so the dashes don't shimmer. */
    char buf[8 + LOCK_PIN_MAX_LEN + 2 + 1];   /* "[PIN: " + mask + "]" */
    int  bi = 0;
    const char *prefix = "[PIN: ";
    while (*prefix) buf[bi++] = *prefix++;

    /* Mask: visible asterisks for entered digits, underscores for the
     * remaining slots up to LOCK_PIN_MIN_LEN. After MIN_LEN we show
     * exactly what's entered (no trailing slots) so longer PINs read
     * naturally. */
    int slots = (g_entry_len > LOCK_PIN_MIN_LEN)
                ? g_entry_len : LOCK_PIN_MIN_LEN;
    for (int i = 0; i < slots; i++) {
        buf[bi++] = (i < g_entry_len) ? '*' : '_';
    }
    buf[bi++] = ']';
    buf[bi] = '\0';

    int prompt_w = bi * 8;
    int px = cx + (CARD_W - prompt_w) / 2;
    int py = cy + 96;
    font_draw(px, py, buf, FONT_BOOT, 16, 0xFFE2E8F0);

    /* Hint line. */
    const char *hint = (g_failed_attempts > 0)
        ? "Wrong PIN -- try again. Press Enter to validate."
        : "Enter PIN. Press Enter to validate.";
    int hint_w = ls_strlen(hint) * 8;
    int hx = cx + (CARD_W - hint_w) / 2;
    int hy = cy + 144;
    font_draw(hx, hy, hint, FONT_BOOT, 16, 0xFF94A3B8);
}
