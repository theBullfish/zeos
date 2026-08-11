/*
 * Zeos -- Idle-driven lock-on-idle.
 *
 * A single TSC stamp (g_last_input_tsc) is bumped by every input
 * source; CHAIN_IDLE.idle_check consults it every ~120ms. State
 * transitions emit lock screen show/repaint, compositor dim, and
 * scanout-blank actions. All thresholds live in VAULT under /idle/.
 */

#include "idle.h"
#include "lockscreen.h"
#include "chain.h"
#include "chain_registry.h"
#include "compositor.h"
#include "fb.h"
#include "vault.h"
#include "timer.h"
#include "kprint.h"
#include "cursor.h"

/* ── Persisted thresholds (defaults applied on first boot) ─────── */

#define IDLE_DEFAULT_DIM_SECS   60u
#define IDLE_DEFAULT_LOCK_SECS  120u
#define IDLE_DEFAULT_BLANK_SECS 300u

static uint32_t g_dim_secs   = IDLE_DEFAULT_DIM_SECS;
static uint32_t g_lock_secs  = IDLE_DEFAULT_LOCK_SECS;
static uint32_t g_blank_secs = IDLE_DEFAULT_BLANK_SECS;

/* ── State ─────────────────────────────────────────────────────── */

static volatile uint64_t g_last_input_tsc;
static idle_state_t      g_state = IDLE_ACTIVE;
static int               g_chain_id = -1;
static int               g_blank_painted;     /* tracks whether the BLANKED
                                                 fb fill has been applied */

/* ── Config helpers ────────────────────────────────────────────── */

static void idle_load_config(void)
{
    uint32_t v = 0;

    if (vault_load_config("/idle/dim_secs", &v, sizeof(v)) == (int)sizeof(v)
        && v > 0 && v < 86400u) {
        g_dim_secs = v;
    }
    if (vault_load_config("/idle/lock_secs", &v, sizeof(v)) == (int)sizeof(v)
        && v > 0 && v < 86400u) {
        g_lock_secs = v;
    }
    if (vault_load_config("/idle/blank_secs", &v, sizeof(v)) == (int)sizeof(v)
        && v > 0 && v < 86400u) {
        g_blank_secs = v;
    }
}

static void idle_save_one(const char *key, uint32_t v)
{
    (void)vault_save_config(key, &v, sizeof(v));
}

/* ── Public API ────────────────────────────────────────────────── */

void idle_mark_active(void)
{
    g_last_input_tsc = timer_read_tsc();

    /* If we're past ACTIVE, the input event itself opens the lock
     * prompt (LOCKED) or repaints over a blanked screen. We never
     * unlock automatically -- the only path back to ACTIVE is a
     * successful PIN entry through lockscreen_input(). */
    if (g_state == IDLE_BLANKED) {
        g_state = IDLE_LOCKED;
        g_blank_painted = 0;
        lockscreen_show();
        compositor_dirty_all();
    } else if (g_state == IDLE_DIMMED) {
        /* User is still active -- back to ACTIVE. The dim overlay is
         * a compositor side-effect, so a redraw clears it. */
        g_state = IDLE_ACTIVE;
        compositor_dirty_all();
    }
}

/* Force transition LOCKED/BLANKED -> ACTIVE. Called by the lock screen
 * after a successful PIN. We cannot rely on idle_mark_active() to do
 * this because that path deliberately refuses to step backward out of
 * LOCKED -- only an authenticated unlock should clear the lock state. */
void idle_force_unlock(void)
{
    g_state = IDLE_ACTIVE;
    g_blank_painted = 0;
    g_last_input_tsc = timer_read_tsc();
    compositor_dirty_all();
    if (g_chain_id >= 0) {
        chain_t *c = chain_get(g_chain_id);
        if (c) c->vault_version++;
    }
}

idle_state_t idle_get_state(void) { return g_state; }
uint32_t     idle_dim_secs(void)   { return g_dim_secs; }
uint32_t     idle_lock_secs(void)  { return g_lock_secs; }
uint32_t     idle_blank_secs(void) { return g_blank_secs; }

uint64_t idle_seconds_since_input(void)
{
    uint64_t freq = timer_tsc_freq();
    if (freq == 0) return 0;
    uint64_t now = timer_read_tsc();
    if (now <= g_last_input_tsc) return 0;
    return (now - g_last_input_tsc) / freq;
}

int idle_chain_id(void) { return g_chain_id; }

int idle_set_dim(uint32_t secs)
{
    if (secs == 0 || secs >= 86400u) return -1;
    g_dim_secs = secs;
    idle_save_one("/idle/dim_secs", secs);
    return 0;
}

int idle_set_lock(uint32_t secs)
{
    if (secs == 0 || secs >= 86400u) return -1;
    g_lock_secs = secs;
    idle_save_one("/idle/lock_secs", secs);
    return 0;
}

int idle_set_blank(uint32_t secs)
{
    if (secs == 0 || secs >= 86400u) return -1;
    g_blank_secs = secs;
    idle_save_one("/idle/blank_secs", secs);
    return 0;
}

void idle_force_lock(void)
{
    if (g_state == IDLE_LOCKED || g_state == IDLE_BLANKED) return;
    g_state = IDLE_LOCKED;
    g_blank_painted = 0;
    lockscreen_show();
    compositor_dirty_all();

    if (g_chain_id >= 0) {
        chain_t *c = chain_get(g_chain_id);
        if (c) c->vault_version++;
    }
}

/* ── Resolves ──────────────────────────────────────────────────── */

/* Stage 1: read seconds-since-input and decide the target state. */
static void idle_check_resolve(chain_node_t *self, void *input, void *output)
{
    (void)self; (void)input;
    uint32_t *out = (uint32_t *)output;
    uint64_t dt = idle_seconds_since_input();

    idle_state_t target = IDLE_ACTIVE;
    if      (dt >= g_blank_secs) target = IDLE_BLANKED;
    else if (dt >= g_lock_secs)  target = IDLE_LOCKED;
    else if (dt >= g_dim_secs)   target = IDLE_DIMMED;

    /* Once locked, never auto-step backward toward DIMMED/ACTIVE --
     * only a successful PIN can unlock. We *do* allow forward motion
     * (LOCKED -> BLANKED) and BLANKED-on-input promotion (handled in
     * idle_mark_active). */
    if ((g_state == IDLE_LOCKED  && target < IDLE_LOCKED) ||
        (g_state == IDLE_BLANKED && target < IDLE_BLANKED)) {
        target = g_state;
    }

    if (out) *out = (uint32_t)target;
}

/* Stage 2: apply the transition (paint dim overlay, show lock screen,
 * blank fb). */
static void idle_state_transition_resolve(chain_node_t *self,
                                          void *input, void *output)
{
    (void)self;
    uint32_t target = input ? *(uint32_t *)input : (uint32_t)g_state;
    uint32_t *out = (uint32_t *)output;

    if ((idle_state_t)target == g_state) {
        if (g_state == IDLE_DIMMED) {
            /* Repaint the dim overlay -- compositor may have cleared
             * it during a normal redraw. The cheapest way to keep it
             * sticky is to flag the fb dirty so the post-overlay hook
             * paints again. The compositor's idle skip means this
             * costs nothing when nothing changed. */
        } else if (g_state == IDLE_BLANKED && !g_blank_painted) {
            fb_clear(0xFF000000);
            g_blank_painted = 1;
        }
        if (out) *out = target;
        return;
    }

    /* Real transition. */
    idle_state_t prev = g_state;
    g_state = (idle_state_t)target;

    switch (g_state) {
    case IDLE_ACTIVE:
        compositor_dirty_all();
        break;
    case IDLE_DIMMED:
        compositor_dirty_all();   /* ensures dim_post_overlay runs */
        break;
    case IDLE_LOCKED:
        if (prev == IDLE_BLANKED) g_blank_painted = 0;
        lockscreen_show();
        compositor_dirty_all();
        break;
    case IDLE_BLANKED:
        fb_clear(0xFF000000);
        g_blank_painted = 1;
        break;
    }

    if (out) *out = target;
}

/* Stage 3: emit a chain-level event so MasQ/B3 see a vault_version
 * bump on every transition. The chain registry / persistence layers
 * already capture vault_version in the snapshot. */
static int g_last_emitted_state = -1;

static void idle_emit_event_resolve(chain_node_t *self,
                                    void *input, void *output)
{
    (void)self;
    uint32_t st = input ? *(uint32_t *)input : (uint32_t)g_state;
    uint32_t *out = (uint32_t *)output;

    if ((int)st != g_last_emitted_state) {
        g_last_emitted_state = (int)st;
        if (g_chain_id >= 0) {
            chain_t *c = chain_get(g_chain_id);
            if (c) c->vault_version++;
        }
        kputs("[idle] state -> ");
        switch ((idle_state_t)st) {
        case IDLE_ACTIVE:  kputs("ACTIVE\n");  break;
        case IDLE_DIMMED:  kputs("DIMMED\n");  break;
        case IDLE_LOCKED:  kputs("LOCKED\n");  break;
        case IDLE_BLANKED: kputs("BLANKED\n"); break;
        }
    }
    if (out) *out = st;
}

/* ── Registration ──────────────────────────────────────────────── */

int idle_chain_register(int parent_chain_id)
{
    /* Defaults first, then VAULT (persisted overrides). */
    idle_load_config();

    /* Treat startup as recent input so we don't immediately dim/lock
     * a freshly-booted system. */
    g_last_input_tsc = timer_read_tsc();
    g_state = IDLE_ACTIVE;
    g_blank_painted = 0;

    g_chain_id = chain_create("idle", parent_chain_id, MASQ_INTERNAL);
    if (g_chain_id < 0) return -1;

    chain_add_node(g_chain_id, "idle_check",
                   "idle_tick", "idle_target",
                   idle_check_resolve);
    chain_add_node(g_chain_id, "state_transition",
                   "idle_target", "idle_state",
                   idle_state_transition_resolve);
    chain_add_node(g_chain_id, "emit_event",
                   "idle_state", "idle_event",
                   idle_emit_event_resolve);

    chain_t *c = chain_get(g_chain_id);
    if (c) {
        /* ~120 ticks ~ 120ms at the measured 1000 tps quantum. */
        c->resolve_interval_ticks = 120;
    }
    return g_chain_id;
}

/* ── Compositor post-overlay hook ──────────────────────────────────
 *
 * Called once per composited frame, AFTER all surface layers + cursor
 * have been drawn. We paint the DIMMED tint here so it overlays
 * everything else, and (re)paint the lock screen on top of the dim
 * tint when LOCKED. BLANKED is handled directly in the transition
 * resolve since fb_clear is already a black flood. */
void idle_post_overlay(void)
{
    switch (g_state) {
    case IDLE_ACTIVE:
        return;
    case IDLE_DIMMED: {
        /* 70% black blend leaves ~30% of original brightness, matching
         * the spec's "30% via brightness_set if available, else darker
         * compositor overlay". */
        uint32_t w = fb_width();
        uint32_t h = fb_height();
        fb_rect_blend(0, 0, (int)w, (int)h, 0xB3000000);
        return;
    }
    case IDLE_LOCKED:
        /* Dim the background first, then paint the lock UI on top. */
        {
            uint32_t w = fb_width();
            uint32_t h = fb_height();
            fb_rect_blend(0, 0, (int)w, (int)h, 0xCC000000);
        }
        lockscreen_draw();
        return;
    case IDLE_BLANKED:
        if (!g_blank_painted) {
            fb_clear(0xFF000000);
            g_blank_painted = 1;
        }
        return;
    }
}
