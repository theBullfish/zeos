/*
 * Idle-driven lock screen. Input timestamps drive state transitions
 * across ACTIVE / DIMMED / LOCKED / BLANKED. PIN-only initially;
 * password / biometrics are future work.
 *
 * The PIN is stored in VAULT at /lock/pin (ASCII digits, NUL-padded).
 * If /lock/pin is missing, the overlay enters enrollment mode
 * ("Set a PIN" + confirm) instead of validation mode.
 *
 * Drawing uses font_draw(FONT_BOOT, ...) so the overlay works before
 * the TTF cache is populated and during the lock-from-cold path.
 *
 * Cold-boot login: same PIN overlay runs at boot if /lock/cold-boot-required
 * is set. First-ever boot prompts for PIN enrollment.
 */

#include "lockscreen.h"
#include "vault.h"
#include "fb.h"
#include "font.h"
#include "kprint.h"
#include "timer.h"
#include "chain.h"
#include "chain_registry.h"
#include "cfa_handle.h"
#include "compositor.h"
#include "idle.h"
#include "keyboard.h"
#include "serial.h"

#include <stdint.h>

#define LOCK_PIN_PATH        "/lock/pin"
#define LOCK_COLD_BOOT_PATH  "/lock/cold-boot-required"
#define LOCK_PIN_MIN_LEN     4
#define LOCK_PIN_MAX_LEN     16
#define LOCK_DEFAULT_PIN     "0000"

#define LOCK_ENTRY_BUF       (LOCK_PIN_MAX_LEN + 1)

/* Enrollment phases for first-boot ("Set a PIN") flow. */
#define LOCK_ENROLL_OFF      0
#define LOCK_ENROLL_PICK     1   /* prompt: "Set a PIN" */
#define LOCK_ENROLL_CONFIRM  2   /* prompt: "Confirm PIN" */

/* The PIN buffers are allocated as static byte arrays then wrapped in
 * CFA handles at MASQ_SOVEREIGN tier. Direct pointer access is only
 * permitted via cfa_resolve(); REFERENCE/INTERNAL observers see NULL.
 *
 * `g_stored_pin_buf` holds the user's enrolled PIN (NUL-padded ASCII).
 * `g_entry_buf`      holds the PIN currently being typed at the gate.
 * `g_enroll_first_buf` holds the first half of the enrollment confirm.
 *
 * The handles are created lazily on first use (after CHAIN_IDLE has
 * registered, so the SOVEREIGN-derived CFA address has a real parent
 * to inherit from). Until then the raw arrays are accessed directly --
 * which is fine because we're still in single-threaded boot.
 */
static char         g_stored_pin_buf[LOCK_ENTRY_BUF];
static char         g_entry_buf[LOCK_ENTRY_BUF];
static char         g_enroll_first_buf[LOCK_ENTRY_BUF];
static cfa_handle_t g_stored_pin_h;
static cfa_handle_t g_entry_h;
static cfa_handle_t g_enroll_first_h;
static int          g_entry_len;
static int          g_active;
static int          g_flash_frames;          /* >0 = render red flash next draw */
static int          g_enroll_phase;
static uint32_t     g_failed_attempts;
static uint32_t     g_unlock_count;

/* Resolve a PIN handle to its raw buffer. Falls back to the underlying
 * static array when:
 *   - the handle isn't wrapped yet (pre-CHAIN_IDLE-registration)
 *   - the current observer can't perceive SOVEREIGN (e.g. inspector
 *     calls into us during a probe). The fallback keeps the boot path
 *     working; see cold_boot_gate for why we cannot afford a NULL here.
 */
static char *pin_resolve(cfa_handle_t h, char *fallback)
{
    if (h == 0) return fallback;
    void *p = cfa_resolve(h);
    return p ? (char *)p : fallback;
}

static void pin_wrap_handles(void)
{
    /* Idempotent. Derive a child CFA from CHAIN_IDLE if available, else
     * a synthetic root rooted at the lockscreen module index 0xLO. */
    cfa_addr_t base;
    chain_t *idle = (idle_chain_id() >= 0) ? chain_get(idle_chain_id()) : (chain_t *)0;
    if (idle) {
        base = cfa_derive(&idle->addr, 0x10C);   /* 'LOC' */
    } else {
        for (int i = 0; i < 8; i++) base.segments[i] = 0;
        base.segments[0] = 0x10C;
        base.depth     = 1;
        base.birth_tsc = timer_read_tsc();
    }
    if (g_stored_pin_h == 0) {
        cfa_addr_t a = cfa_derive(&base, 1);
        g_stored_pin_h = cfa_wrap(g_stored_pin_buf, sizeof(g_stored_pin_buf),
                                  a, MASQ_SOVEREIGN);
    }
    if (g_entry_h == 0) {
        cfa_addr_t a = cfa_derive(&base, 2);
        g_entry_h = cfa_wrap(g_entry_buf, sizeof(g_entry_buf),
                             a, MASQ_SOVEREIGN);
    }
    if (g_enroll_first_h == 0) {
        cfa_addr_t a = cfa_derive(&base, 3);
        g_enroll_first_h = cfa_wrap(g_enroll_first_buf, sizeof(g_enroll_first_buf),
                                    a, MASQ_SOVEREIGN);
    }
}

/* Constant-time PIN compare. Walks the FULL buffer regardless of when
 * the first mismatch occurs so a side-channel observer can't time the
 * comparison to learn the prefix. Both buffers are NUL-padded to
 * LOCK_ENTRY_BUF; we walk the whole thing. */
static int pin_compare_ct(const char *a, const char *b)
{
    unsigned diff = 0;
    for (int i = 0; i < LOCK_ENTRY_BUF; i++) {
        diff |= (unsigned)((unsigned char)a[i] ^ (unsigned char)b[i]);
    }
    return diff == 0;
}

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

/* Retained for symmetry with other helpers; PIN compares now use the
 * constant-time pin_compare_ct() instead. Declared so an inadvertent
 * future call still works even though no current caller exists. */
__attribute__((unused))
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

    /* Make sure the SOVEREIGN-tier handles exist before we touch the
     * underlying buffers. If the wrap fails (table full -- shouldn't
     * happen during boot) we fall back to direct static access via
     * pin_resolve() which returns the raw array. */
    pin_wrap_handles();

    char *stored = pin_resolve(g_stored_pin_h, g_stored_pin_buf);
    /* Zero entire buffer so pin_compare_ct stays well-defined regardless
     * of how short the new PIN ends up being. */
    for (int i = 0; i < LOCK_ENTRY_BUF; i++) stored[i] = '\0';

    int got = vault_read(LOCK_PIN_PATH, buf, sizeof(buf) - 1);
    if (got <= 0) {
        /* No PIN yet -- enrollment will run at the next cold-boot gate
         * (or via `pin <new>` from the shell). */
        kputs("[lockscreen] no PIN set; enrollment will run at next "
              "cold-boot gate or via `pin <new>`.\n");
        return;
    }

    /* Sanitize: keep only leading digits up to MAX_LEN. */
    int n = 0;
    for (int i = 0; i < got && n < LOCK_PIN_MAX_LEN; i++) {
        if (ls_is_digit((char)buf[i])) stored[n++] = (char)buf[i];
        else if (buf[i] == 0) break;
    }
    stored[n] = '\0';
    if (n < LOCK_PIN_MIN_LEN) {
        /* Stored PIN is corrupt -- forget it and require enrollment. */
        kputs("[lockscreen] stored PIN corrupt; clearing.\n");
        for (int i = 0; i < LOCK_ENTRY_BUF; i++) stored[i] = '\0';
        (void)vault_delete(LOCK_PIN_PATH);
    }
}

void lockscreen_init(void)
{
    pin_wrap_handles();
    char *entry = pin_resolve(g_entry_h, g_entry_buf);
    char *first = pin_resolve(g_enroll_first_h, g_enroll_first_buf);
    g_entry_len = 0;
    for (int i = 0; i < LOCK_ENTRY_BUF; i++) entry[i] = '\0';
    for (int i = 0; i < LOCK_ENTRY_BUF; i++) first[i] = '\0';
    g_enroll_phase = LOCK_ENROLL_OFF;
    g_active = 0;
    g_flash_frames = 0;
    g_failed_attempts = 0;
    g_unlock_count = 0;
    lockscreen_load_pin();
}

int lockscreen_pin_configured(void)
{
    char *stored = pin_resolve(g_stored_pin_h, g_stored_pin_buf);
    return ls_strlen(stored) >= LOCK_PIN_MIN_LEN;
}

int lockscreen_cfa_handle_count(void)
{
    int n = 0;
    if (g_stored_pin_h)   n++;
    if (g_entry_h)        n++;
    if (g_enroll_first_h) n++;
    return n;
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

    pin_wrap_handles();
    char *stored = pin_resolve(g_stored_pin_h, g_stored_pin_buf);
    /* Wipe full buffer first so pin_compare_ct's tail is deterministic. */
    for (int i = 0; i < LOCK_ENTRY_BUF; i++) stored[i] = '\0';
    ls_strncpy(stored, new_pin, LOCK_ENTRY_BUF);
    int wrote = vault_write(LOCK_PIN_PATH, stored, (uint32_t)n);
    if (wrote < 0) return -1;
    return 0;
}

int lockscreen_pin_clear(void)
{
    char *stored = pin_resolve(g_stored_pin_h, g_stored_pin_buf);
    for (int i = 0; i < LOCK_ENTRY_BUF; i++) stored[i] = '\0';
    /* vault_delete returns 0 on success or already-absent; either way
     * the in-memory state is now empty. */
    (void)vault_delete(LOCK_PIN_PATH);
    kputs("[lockscreen] PIN cleared; enrollment required on next lock.\n");
    return 0;
}

/* ── Cold-boot login config ───────────────────────────────────── */

int cold_boot_login_required(void)
{
    uint32_t v = 1;  /* default = required */
    int got = vault_load_config(LOCK_COLD_BOOT_PATH, &v, sizeof(v));
    if (got != (int)sizeof(v)) return 1;
    return v ? 1 : 0;
}

int cold_boot_login_set(int enabled)
{
    uint32_t v = enabled ? 1u : 0u;
    return vault_save_config(LOCK_COLD_BOOT_PATH, &v, sizeof(v));
}

/* ── Modal state ──────────────────────────────────────────────── */

void lockscreen_show(void)
{
    pin_wrap_handles();
    char *entry = pin_resolve(g_entry_h, g_entry_buf);
    char *first = pin_resolve(g_enroll_first_h, g_enroll_first_buf);
    g_entry_len = 0;
    for (int i = 0; i < LOCK_ENTRY_BUF; i++) entry[i] = '\0';
    g_active = 1;
    g_flash_frames = 0;
    /* If no PIN is configured, fall through to enrollment instead of
     * trapping the user at a validation prompt that nothing satisfies. */
    if (!lockscreen_pin_configured()) {
        g_enroll_phase = LOCK_ENROLL_PICK;
        for (int i = 0; i < LOCK_ENTRY_BUF; i++) first[i] = '\0';
    } else {
        g_enroll_phase = LOCK_ENROLL_OFF;
    }
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

/* ── Enrollment helpers ──────────────────────────────────────── */

static void enroll_reset_to_pick(void)
{
    char *entry = pin_resolve(g_entry_h, g_entry_buf);
    char *first = pin_resolve(g_enroll_first_h, g_enroll_first_buf);
    g_enroll_phase = LOCK_ENROLL_PICK;
    for (int i = 0; i < LOCK_ENTRY_BUF; i++) first[i] = '\0';
    g_entry_len = 0;
    for (int i = 0; i < LOCK_ENTRY_BUF; i++) entry[i] = '\0';
    g_flash_frames = 12;
}

static void enroll_handle_enter(void)
{
    char *entry = pin_resolve(g_entry_h, g_entry_buf);
    char *first = pin_resolve(g_enroll_first_h, g_enroll_first_buf);

    /* Validate length, store/confirm, then either advance or commit. */
    if (g_entry_len < LOCK_PIN_MIN_LEN || g_entry_len > LOCK_PIN_MAX_LEN) {
        g_flash_frames = 12;
        kputs("[lockscreen] enroll: PIN must be ");
        kput_dec((uint64_t)LOCK_PIN_MIN_LEN);
        kputs("..");
        kput_dec((uint64_t)LOCK_PIN_MAX_LEN);
        kputs(" digits\n");
        g_entry_len = 0;
        for (int i = 0; i < LOCK_ENTRY_BUF; i++) entry[i] = '\0';
        return;
    }

    if (g_enroll_phase == LOCK_ENROLL_PICK) {
        for (int i = 0; i < LOCK_ENTRY_BUF; i++) first[i] = '\0';
        ls_strncpy(first, entry, LOCK_ENTRY_BUF);
        g_entry_len = 0;
        for (int i = 0; i < LOCK_ENTRY_BUF; i++) entry[i] = '\0';
        g_enroll_phase = LOCK_ENROLL_CONFIRM;
        return;
    }

    /* CONFIRM phase. Constant-time compare across full buffer. */
    if (!pin_compare_ct(entry, first)) {
        kputs("[lockscreen] enroll: confirm mismatch; restart\n");
        enroll_reset_to_pick();
        return;
    }

    if (lockscreen_set_pin(first) < 0) {
        kputs("[lockscreen] enroll: set_pin failed; restart\n");
        enroll_reset_to_pick();
        return;
    }

    kputs("[lockscreen] PIN enrolled\n");
    g_enroll_phase = LOCK_ENROLL_OFF;
    g_unlock_count++;
    g_active = 0;
    g_entry_len = 0;
    for (int i = 0; i < LOCK_ENTRY_BUF; i++) entry[i] = '\0';
    for (int i = 0; i < LOCK_ENTRY_BUF; i++) first[i] = '\0';
    g_flash_frames = 0;

    int id = idle_chain_id();
    if (id >= 0) {
        chain_t *cc = chain_get(id);
        if (cc) cc->vault_version++;
    }
    extern void idle_force_unlock(void);
    idle_force_unlock();
}

void lockscreen_input(char c)
{
    if (!g_active) return;

    char *entry  = pin_resolve(g_entry_h,      g_entry_buf);
    char *stored = pin_resolve(g_stored_pin_h, g_stored_pin_buf);

    if (c == '\b') {
        if (g_entry_len > 0) {
            g_entry_len--;
            entry[g_entry_len] = '\0';
        }
        return;
    }

    if (c == '\n' || c == '\r') {
        if (g_entry_len == 0) return;

        if (g_enroll_phase != LOCK_ENROLL_OFF) {
            enroll_handle_enter();
            return;
        }

        if (pin_compare_ct(entry, stored)) {
            /* Unlock. */
            g_unlock_count++;
            g_active = 0;
            g_entry_len = 0;
            for (int i = 0; i < LOCK_ENTRY_BUF; i++) entry[i] = '\0';
            g_flash_frames = 0;
            kputs("[lockscreen] unlocked\n");

            /* Bump CHAIN_IDLE vault_version on success too. */
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
        for (int i = 0; i < LOCK_ENTRY_BUF; i++) entry[i] = '\0';

        kputs("[lockscreen] failed PIN attempt #");
        kput_dec((uint64_t)g_failed_attempts);
        kputc('\n');

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
    entry[g_entry_len++] = c;
    entry[g_entry_len] = '\0';
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

    /* Title varies by mode: validation vs enrollment. */
    const char *title = "ZEOS  Locked";
    if (g_enroll_phase == LOCK_ENROLL_PICK)    title = "ZEOS  Set a PIN";
    else if (g_enroll_phase == LOCK_ENROLL_CONFIRM) title = "ZEOS  Confirm PIN";
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
    const char *hint;
    if (g_enroll_phase == LOCK_ENROLL_PICK) {
        hint = "Choose a 4-16 digit PIN. Press Enter to continue.";
    } else if (g_enroll_phase == LOCK_ENROLL_CONFIRM) {
        hint = "Re-enter the same PIN to confirm. Press Enter.";
    } else if (g_failed_attempts > 0) {
        hint = "Wrong PIN -- try again. Press Enter to validate.";
    } else {
        hint = "Enter PIN. Press Enter to validate.";
    }
    int hint_w = ls_strlen(hint) * 8;
    int hx = cx + (CARD_W - hint_w) / 2;
    int hy = cy + 144;
    font_draw(hx, hy, hint, FONT_BOOT, 16, 0xFF94A3B8);
}

/* ── Cold-boot login gate ─────────────────────────────────────── */

/*
 * Synchronous boot-time gate. Runs between chain_registry_init and
 * scheduler_run. Shows the same overlay used by idle lock; the only
 * differences from the idle path are:
 *   1. The overlay is engaged unconditionally (not via the IDLE chain).
 *   2. We hand-pump chain_registry_tick() + compositor_frame() until
 *      the user has either validated their PIN or completed enrollment.
 *
 * Input flows the existing way: keyboard.c routes ASCII to
 * lockscreen_input() while the overlay is active, so the shell pump
 * never sees the bytes typed at the gate.
 */
void lockscreen_run_cold_boot_gate(void)
{
    lockscreen_show();

    if (g_enroll_phase != LOCK_ENROLL_OFF) {
        kputs("[lockscreen] cold-boot gate: PIN enrollment\n");
    } else {
        kputs("[lockscreen] cold-boot gate: PIN required\n");
    }

    compositor_dirty_all();

    /* Pump until the overlay clears itself. The scheduler is not running
     * yet -- mde_resolve_all gates many chains by scheduler_tick_count,
     * which is still 0 -- so we drive input drains directly here:
     *
     *   1. keyboard_chain_drain() drains the PS/2 / USB-HID scancode
     *      ring through process_scancode(), which routes ASCII to
     *      lockscreen_input() while the overlay is active.
     *   2. serial_rx_pop() drains COM1 RX through keyboard_inject_char(),
     *      which has the same lockscreen-gating fast path.
     *   3. keyboard_try_getc() drains any leftover ASCII that landed in
     *      kb_buf before the lock screen claimed the modal -- belt and
     *      suspenders.
     *   4. compositor_frame() paints the overlay each loop iteration so
     *      the user actually sees what they're typing into.
     *   5. HLT until the next IRQ (PIT 1kHz or serial RX) to keep the
     *      CPU idle while we wait. */
    /* Paint the overlay once into the framebuffer. We deliberately do
     * NOT use compositor_frame() here because the compositor pipeline
     * relies on chains that the scheduler hasn't activated yet; in
     * tests we observed the gate stalling on its second iteration when
     * compositor_frame ran the full chain pass. The overlay is a simple
     * centered card -- one paint plus per-input repaints is enough. */
    extern void idle_post_overlay(void);

    fb_clear(0xFF0F1115);
    lockscreen_draw();

    while (g_active) {
        (void)keyboard_chain_drain();

        /* serial_rx_pending() also tops up the RX ring from the UART
         * FIFO so polling works on hosts where the COM1 IRQ never
         * fires (notably some QEMU configurations). */
        (void)serial_rx_pending();
        uint8_t b;
        int rx_any = 0;
        while (serial_rx_pop(&b)) {
            char c;
            if      (b == 0x0D || b == 0x0A) c = '\n';
            else if (b == 0x7F || b == 0x08) c = '\b';
            else if (b < 0x20 || b > 0x7E)   continue;
            else                              c = (char)b;
            keyboard_inject_char(c);
            rx_any = 1;
        }

        int kb_any = 0;
        char c;
        while (keyboard_try_getc(&c)) {
            if (lockscreen_active()) {
                lockscreen_input(c);
                kb_any = 1;
            } else {
                break;
            }
        }

        if (rx_any || kb_any) {
            /* Repaint the masked entry on every keystroke. */
            fb_clear(0xFF0F1115);
            lockscreen_draw();
        }

        /* Cheap busy-poll. We don't depend on the IRQ wake here because
         * some boot paths arrive at the gate with the next IRQ delayed
         * (observed: HLT producing zero wakeups in QEMU stdio mode).
         * The spin keeps CPU usage bounded without a hard sleep. */
        for (volatile int i = 0; i < 100000; i++) { /* spin */ }
    }

    kputs("[lockscreen] cold-boot gate: cleared\n");
    compositor_dirty_all();
}
