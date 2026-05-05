/*
 * Unified settings registry. Every config knob in the system registers
 * a getter/setter pair. Shell `settings` command is the single source
 * of truth for "what can the user change?" Each module continues to
 * own its own VAULT persistence — we just expose the surface.
 */

#include "settings_registry.h"
#include "kprint.h"
#include "chain.h"
#include "chain_registry.h"

#include "hda.h"
#include "brightness.h"
#include "idle.h"
#include "lockscreen.h"
#include "timeofday.h"
#include "access.h"
#include "power_buttons.h"
#include "gpu_compute.h"
#include "mde_chain.h"
#include "wm.h"
#include "workspaces.h"

#include <stdint.h>

/* ── Internal table ───────────────────────────────────────────── */

static const settings_entry_t *g_table[SETTINGS_REGISTRY_MAX];
static int                     g_count = 0;

/* ── String helpers (no libc) ─────────────────────────────────── */

static int sr_strlen(const char *s)
{
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static int sr_streq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

static void sr_strcpy(char *dst, int dst_len, const char *src)
{
    int i = 0;
    if (dst_len <= 0) return;
    if (src) {
        while (src[i] && i < dst_len - 1) { dst[i] = src[i]; i++; }
    }
    dst[i] = '\0';
}

static int sr_atoi(const char *s, int *out)
{
    if (!s || !*s) return -1;
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') { s++; }
    if (!*s) return -1;
    int v = 0;
    while (*s) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (*s - '0');
        s++;
    }
    *out = v * sign;
    return 0;
}

static int sr_itoa(int v, char *out, int out_len)
{
    if (out_len <= 0) return -1;
    char tmp[16];
    int n = 0, neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) tmp[n++] = '0';
    while (v > 0 && n < (int)sizeof(tmp)) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    int i = 0;
    if (neg && i < out_len - 1) out[i++] = '-';
    while (n > 0 && i < out_len - 1) out[i++] = tmp[--n];
    out[i] = '\0';
    return i;
}

/* parse one of the labels in "a|b|c" — returns index, or -1 on miss */
static int sr_enum_index(const char *labels, const char *value)
{
    if (!labels || !value) return -1;
    int idx = 0;
    const char *p = labels;
    const char *seg_start = p;
    while (1) {
        if (*p == '|' || *p == '\0') {
            int seg_len = (int)(p - seg_start);
            int v_len = sr_strlen(value);
            if (seg_len == v_len) {
                int match = 1;
                for (int i = 0; i < seg_len; i++) {
                    if (seg_start[i] != value[i]) { match = 0; break; }
                }
                if (match) return idx;
            }
            if (*p == '\0') break;
            idx++;
            seg_start = p + 1;
        }
        p++;
    }
    return -1;
}

/* ── Public registry API ─────────────────────────────────────── */

int settings_register(const settings_entry_t *entry)
{
    if (!entry || !entry->name) return -1;
    if (g_count >= SETTINGS_REGISTRY_MAX) return -1;
    /* Reject duplicates */
    for (int i = 0; i < g_count; i++) {
        if (sr_streq(g_table[i]->name, entry->name)) return -1;
    }
    g_table[g_count++] = entry;
    return 0;
}

const settings_entry_t *settings_find(const char *name)
{
    if (!name) return 0;
    for (int i = 0; i < g_count; i++) {
        if (sr_streq(g_table[i]->name, name)) return g_table[i];
    }
    return 0;
}

int settings_get(const char *name, char *out, int out_len)
{
    const settings_entry_t *e = settings_find(name);
    if (!e || out_len <= 0) return -1;
    out[0] = '\0';
    if (e->flags & SETTINGS_FLAG_WRITEONLY) {
        sr_strcpy(out, out_len, "***");
        return 0;
    }
    if (!e->getter) return -1;
    return e->getter(out, out_len);
}

/* ── Chain pipeline (CHAIN_SETTINGS) ───────────────────────────── */

#define SETTINGS_OLD_MAX  64
#define SETTINGS_NEW_MAX  64

typedef struct {
    const settings_entry_t *entry;
    char                    name[SETTINGS_NAME_MAX];
    char                    new_val[SETTINGS_NEW_MAX];
    char                    old_val[SETTINGS_OLD_MAX];
    int                     status;       /* 0 = ok, -1 = reject */
    int                     applied;      /* 1 once apply node ran */
} settings_change_t;

static settings_change_t s_chg_pending;
static int               s_chain_settings = -1;
static uint32_t          s_settings_apply_total;

/* String helpers local to chain resolves. */
static void sc_strcpy(char *dst, int dst_len, const char *src)
{
    int i = 0;
    if (dst_len <= 0) return;
    if (src) {
        while (src[i] && i < dst_len - 1) { dst[i] = src[i]; i++; }
    }
    dst[i] = '\0';
}

static int sc_int_pos(const char *s)
{
    int v = 0, any = 0;
    if (!s) return -1;
    if (*s == '-') return -1;
    if (*s == '+') s++;
    while (*s) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (*s - '0');
        any = 1;
        s++;
    }
    return any ? v : -1;
}

static int sc_int_signed(const char *s, int *out)
{
    if (!s || !*s) return -1;
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') { s++; }
    if (!*s) return -1;
    int v = 0;
    while (*s) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (*s - '0');
        s++;
    }
    *out = v * sign;
    return 0;
}

static int kind_validate(settings_kind_t k, const char *v, const char *enum_labels)
{
    switch (k) {
    case SK_STRING:    return v ? 0 : -1;
    case SK_INT: {
        int x;
        return sc_int_signed(v, &x) == 0 ? 0 : -1;
    }
    case SK_INT_PCT: {
        int x = sc_int_pos(v);
        return (x >= 0 && x <= 100) ? 0 : -1;
    }
    case SK_INT_SECS: {
        int x = sc_int_pos(v);
        return (x >= 0) ? 0 : -1;
    }
    case SK_BOOL: {
        int x;
        if (sc_int_signed(v, &x) != 0) return -1;
        return (x == 0 || x == 1) ? 0 : -1;
    }
    case SK_ENUM:
        return sr_enum_index(enum_labels, v) >= 0 ? 0 : -1;
    }
    return -1;
}

static void settings_change_request_resolve(chain_node_t *self,
                                            void *input, void *output)
{
    (void)self; (void)input;
    settings_change_t *out = (settings_change_t *)output;
    *out = s_chg_pending;
}

static void settings_validate_resolve(chain_node_t *self,
                                      void *input, void *output)
{
    (void)self;
    settings_change_t *in  = (settings_change_t *)input;
    settings_change_t *out = (settings_change_t *)output;
    *out = *in;

    const settings_entry_t *e = in->entry;
    if (!e || (e->flags & SETTINGS_FLAG_READONLY) || !e->setter) {
        out->status = -1;
        s_chg_pending.status = -1;
        return;
    }
    if (kind_validate(e->kind, in->new_val, e->enum_labels) != 0) {
        out->status = -1;
        s_chg_pending.status = -1;
        return;
    }
    out->status = 0;
}

static void settings_apply_resolve(chain_node_t *self,
                                   void *input, void *output)
{
    (void)self;
    settings_change_t *in  = (settings_change_t *)input;
    settings_change_t *out = (settings_change_t *)output;
    *out = *in;
    if (in->status != 0) return;

    const settings_entry_t *e = in->entry;

    /* Capture prior value for the {key, old, new} provenance tuple. */
    if (e->getter && (e->flags & SETTINGS_FLAG_WRITEONLY) == 0) {
        char prev[SETTINGS_OLD_MAX];
        prev[0] = '\0';
        if (e->getter(prev, (int)sizeof(prev)) == 0) {
            sc_strcpy(out->old_val, SETTINGS_OLD_MAX, prev);
            sc_strcpy(s_chg_pending.old_val, SETTINGS_OLD_MAX, prev);
        }
    } else {
        sc_strcpy(out->old_val, SETTINGS_OLD_MAX, "***");
        sc_strcpy(s_chg_pending.old_val, SETTINGS_OLD_MAX, "***");
    }

    int rc = e->setter(in->new_val);
    if (rc != 0) {
        out->status = -1;
        s_chg_pending.status = -1;
        return;
    }
    out->applied = 1;
    s_chg_pending.applied = 1;
}

static void settings_record_resolve(chain_node_t *self,
                                    void *input, void *output)
{
    (void)self;
    settings_change_t *in  = (settings_change_t *)input;
    settings_change_t *out = (settings_change_t *)output;
    *out = *in;
    if (!in->applied || in->status != 0) return;

    /* Bump CHAIN_SETTINGS.vault_version with the {key, old, new} tuple
     * implicit in the chain resolve's TSC + this counter. masq_journal
     * (block-chain term of art) records by vault_version delta; for
     * settings the equivalent is the chain's vault_version itself. */
    if (s_chain_settings >= 0) {
        chain_t *c = chain_get(s_chain_settings);
        if (c) c->vault_version++;
    }
    s_settings_apply_total++;

    /* Mirror the new value into s_chg_pending so settings_set() can
     * read the chain-resolved status when it returns. */
    s_chg_pending.applied = 1;
    s_chg_pending.status  = 0;
}

int settings_chain_register(int parent_chain_id)
{
    if (s_chain_settings >= 0) {
        CHAIN_SETTINGS = s_chain_settings;
        return s_chain_settings;
    }
    int id = chain_create("settings", parent_chain_id, MASQ_INTERNAL);
    if (id < 0) return -1;
    chain_add_node(id, "setting_change_request",
                   "settings_request", "settings_change",
                   settings_change_request_resolve);
    chain_add_node(id, "validate",
                   "settings_change", "settings_change",
                   settings_validate_resolve);
    chain_add_node(id, "apply",
                   "settings_change", "settings_change",
                   settings_apply_resolve);
    chain_add_node(id, "record",
                   "settings_change", "settings_change",
                   settings_record_resolve);
    s_chain_settings = id;
    CHAIN_SETTINGS   = id;
    return id;
}

uint32_t settings_chain_apply_total(void) { return s_settings_apply_total; }

int settings_set(const char *name, const char *value)
{
    const settings_entry_t *e = settings_find(name);
    if (!e) return -1;
    if (e->flags & SETTINGS_FLAG_READONLY) return -1;
    if (!e->setter) return -1;
    if (!value) return -1;

    /* Build the chain request. */
    s_chg_pending.entry   = e;
    s_chg_pending.status  = 0;
    s_chg_pending.applied = 0;
    sc_strcpy(s_chg_pending.name,    SETTINGS_NAME_MAX, name);
    sc_strcpy(s_chg_pending.new_val, SETTINGS_NEW_MAX,  value);
    s_chg_pending.old_val[0] = '\0';

    if (s_chain_settings >= 0) {
        (void)chain_resolve(s_chain_settings);
        return (s_chg_pending.applied && s_chg_pending.status == 0) ? 0 : -1;
    }

    /* Pre-registration fallback: validate + apply directly so boot-time
     * setters still work. After chain_registry_init, every set runs
     * through the pipeline. */
    if (kind_validate(e->kind, value, e->enum_labels) != 0) return -1;
    return e->setter(value);
}

int settings_enumerate(settings_walk_cb cb, void *user)
{
    if (!cb) return 0;
    for (int i = 0; i < g_count; i++) {
        if (cb(g_table[i], user)) return i + 1;
    }
    return g_count;
}

int settings_count(void) { return g_count; }

int settings_live_count(void)
{
    int n = 0;
    for (int i = 0; i < g_count; i++) {
        if ((g_table[i]->flags & SETTINGS_FLAG_STUB) == 0) n++;
    }
    return n;
}

const char *settings_kind_label(settings_kind_t k)
{
    switch (k) {
        case SK_STRING:   return "STRING";
        case SK_INT:      return "INT";
        case SK_INT_PCT:  return "INT_PCT";
        case SK_INT_SECS: return "INT_SECS";
        case SK_BOOL:     return "BOOL";
        case SK_ENUM:     return "ENUM";
    }
    return "?";
}

/* ── Source-module bridges ─────────────────────────────────────
 *
 * Each setting below has a tiny getter/setter pair that translates
 * between the registry's string interface and the source module's
 * native API. Persistence lives entirely in the source module — we
 * never write to VAULT directly here.
 */

/* audio.volume */
static int g_audio_volume(char *out, int n) {
    return sr_itoa(hda_audio_get_volume(), out, n) >= 0 ? 0 : -1;
}
static int s_audio_volume(const char *v) {
    int x; if (sr_atoi(v, &x) != 0) return -1;
    if (x < 0 || x > 100) return -1;
    hda_audio_set_volume(x);
    (void)hda_audio_save_to_vault();
    return 0;
}

/* audio.muted */
static int g_audio_muted(char *out, int n) {
    return sr_itoa(hda_audio_get_muted() ? 1 : 0, out, n) >= 0 ? 0 : -1;
}
static int s_audio_muted(const char *v) {
    int x; if (sr_atoi(v, &x) != 0) return -1;
    hda_audio_set_muted(x ? 1 : 0);
    (void)hda_audio_save_to_vault();
    return 0;
}

/* display.brightness */
static int g_display_brightness(char *out, int n) {
    int b = brightness_get();
    if (b < 0) { sr_strcpy(out, n, "n/a"); return 0; }
    return sr_itoa(b, out, n) >= 0 ? 0 : -1;
}
static int s_display_brightness(const char *v) {
    int x; if (sr_atoi(v, &x) != 0) return -1;
    if (x < 0 || x > 100) return -1;
    int rc = brightness_set(x);
    if (rc < 0) return -1;
    (void)brightness_save_to_vault();
    return 0;
}

/* idle.dim_secs / lock_secs / blank_secs */
static int g_idle_dim(char *o, int n)   { return sr_itoa((int)idle_dim_secs(),   o, n) >= 0 ? 0 : -1; }
static int g_idle_lock(char *o, int n)  { return sr_itoa((int)idle_lock_secs(),  o, n) >= 0 ? 0 : -1; }
static int g_idle_blank(char *o, int n) { return sr_itoa((int)idle_blank_secs(), o, n) >= 0 ? 0 : -1; }
static int s_idle_dim(const char *v) {
    int x; if (sr_atoi(v, &x) != 0 || x < 0) return -1;
    return idle_set_dim((uint32_t)x);
}
static int s_idle_lock(const char *v) {
    int x; if (sr_atoi(v, &x) != 0 || x < 0) return -1;
    return idle_set_lock((uint32_t)x);
}
static int s_idle_blank(const char *v) {
    int x; if (sr_atoi(v, &x) != 0 || x < 0) return -1;
    return idle_set_blank((uint32_t)x);
}

/* lock.pin (write-only) */
static int g_lock_pin(char *out, int n) { sr_strcpy(out, n, "***"); return 0; }
static int s_lock_pin(const char *v) {
    if (!v) return -1;
    return lockscreen_set_pin(v);
}

/* lock.cold_boot_required */
static int g_lock_cold(char *out, int n) {
    return sr_itoa(cold_boot_login_required() ? 1 : 0, out, n) >= 0 ? 0 : -1;
}
static int s_lock_cold(const char *v) {
    int x; if (sr_atoi(v, &x) != 0) return -1;
    return cold_boot_login_set(x ? 1 : 0);
}

/* trash.auto_empty_days — there is no live setter (CHAIN_TRASH_GC
 * uses a baked-in 30-day cutoff). Expose as readonly until a tunable
 * lands. */
static int g_trash_auto_empty(char *out, int n) {
    return sr_itoa(30, out, n) >= 0 ? 0 : -1;
}
static int s_trash_auto_empty(const char *v) { (void)v; return -1; }

/* power.button_action / lid_close_action / lid_open_action — back
 * onto power_buttons.c. */
static int g_power_btn(char *out, int n) {
    sr_strcpy(out, n, power_action_label(power_button_action()));
    return 0;
}
static int g_power_lid_close(char *out, int n) {
    sr_strcpy(out, n, power_action_label(lid_close_action()));
    return 0;
}
static int g_power_lid_open(char *out, int n) {
    sr_strcpy(out, n, power_action_label(lid_open_action()));
    return 0;
}
static int s_power_btn(const char *v) {
    power_action_t a;
    if (power_action_parse(v, &a) != 0) return -1;
    return power_button_set_action(a);
}
static int s_power_lid_close(const char *v) {
    power_action_t a;
    if (power_action_parse(v, &a) != 0) return -1;
    return lid_close_set_action(a);
}
static int s_power_lid_open(const char *v) {
    power_action_t a;
    if (power_action_parse(v, &a) != 0) return -1;
    return lid_open_set_action(a);
}

/* time.tz_offset_minutes — exposed via the global hook in timeofday.h. */
static int g_tz(char *out, int n) {
    return sr_itoa(tod_tz_offset_minutes, out, n) >= 0 ? 0 : -1;
}
static int s_tz(const char *v) {
    int x; if (sr_atoi(v, &x) != 0) return -1;
    if (x < -24*60 || x > 24*60) return -1;
    tod_tz_offset_minutes = x;
    return 0;
}

/* network.dhcp_enabled — no toggle exists yet; expose readonly. */
static int g_net_dhcp(char *out, int n) { sr_itoa(1, out, n); return 0; }
static int s_net_dhcp(const char *v)    { (void)v; return -1; }

/* theme.dark_mode — derived from access config. SCHEME_DARK or AUTO
 * with a dark resolve count as on; LIGHT counts as off. Setter flips
 * between SCHEME_DARK and SCHEME_LIGHT. */
static int g_theme_dark(char *out, int n) {
    access_config_t *c = access_get();
    int on = (c && c->scheme != SCHEME_LIGHT) ? 1 : 0;
    return sr_itoa(on, out, n) >= 0 ? 0 : -1;
}
static int s_theme_dark(const char *v) {
    int x; if (sr_atoi(v, &x) != 0) return -1;
    access_set_scheme(x ? SCHEME_DARK : SCHEME_LIGHT);
    access_save();
    return 0;
}

/* mde.select_policy — picks how CHAIN_MDE.device_select balances work
 * across registered backends. Default LEAST_LOADED. Each change bumps
 * CHAIN_MDE.vault_version with old/new (MasQ provenance). */
static int g_mde_policy(char *out, int n) {
    const char *lab = gpu_compute_policy_label(gpu_compute_get_policy());
    sr_strcpy(out, n, lab ? lab : "?");
    /* Lowercase for setting display consistency. */
    for (int i = 0; out[i]; i++) {
        if (out[i] >= 'A' && out[i] <= 'Z') out[i] = (char)(out[i] - 'A' + 'a');
    }
    return 0;
}
static int s_mde_policy(const char *v) {
    if (!v) return -1;
    int newp = -1;
    if (sr_streq(v, "first"))             newp = MDE_SELECT_FIRST;
    else if (sr_streq(v, "round_robin"))  newp = MDE_SELECT_ROUND_ROBIN;
    else if (sr_streq(v, "least_loaded")) newp = MDE_SELECT_LEAST_LOADED;
    else if (sr_streq(v, "shape_aware"))  newp = MDE_SELECT_SHAPE_AWARE;
    if (newp < 0) return -1;
    int oldp = gpu_compute_get_policy();
    if (gpu_compute_set_policy(newp) != 0) return -1;
    if (oldp != newp && CHAIN_MDE >= 0) {
        chain_t *c = chain_get(CHAIN_MDE);
        if (c) {
            c->vault_version++;
            kputs("[mde] select_policy ");
            kputs(gpu_compute_policy_label(oldp));
            kputs(" -> ");
            kputs(gpu_compute_policy_label(newp));
            kputs(" (vault_version=");
            kput_dec((uint64_t)c->vault_version);
            kputs(")\n");
        }
    }
    return 0;
}

/* wm.snap_drag_threshold — pixels from screen edge that trigger a snap
 * preview while dragging a window. */
static int g_wm_snap_drag(char *out, int n) {
    return sr_itoa(wm_get_snap_drag_threshold(), out, n);
}
static int s_wm_snap_drag(const char *v) {
    int x; if (sr_atoi(v, &x) != 0) return -1;
    if (x < 1 || x > 256) return -1;
    wm_set_snap_drag_threshold(x);
    return 0;
}

/* wm.snap_animation_ms — total time hint for the spring-animated snap. */
static int g_wm_snap_anim(char *out, int n) {
    return sr_itoa(wm_get_snap_animation_ms(), out, n);
}
static int s_wm_snap_anim(const char *v) {
    int x; if (sr_atoi(v, &x) != 0) return -1;
    if (x < 30 || x > 2000) return -1;
    wm_set_snap_animation_ms(x);
    return 0;
}

/* wm.workspace_count — number of virtual desktops (1..8, default 4). */
static int g_wm_ws_count(char *out, int n) {
    return sr_itoa(workspaces_get_count(), out, n);
}
static int s_wm_ws_count(const char *v) {
    int x; if (sr_atoi(v, &x) != 0) return -1;
    if (x < 1 || x > 8) return -1;
    workspaces_set_count(x);
    return 0;
}

/* wm.workspace_animation_ms — slide animation total time (default 200). */
static int g_wm_ws_anim(char *out, int n) {
    return sr_itoa(workspaces_get_animation_ms(), out, n);
}
static int s_wm_ws_anim(const char *v) {
    int x; if (sr_atoi(v, &x) != 0) return -1;
    if (x < 30 || x > 2000) return -1;
    workspaces_set_animation_ms(x);
    return 0;
}

/* ── Static entry table. Pointers handed to settings_register. ── */

static const settings_entry_t E_AUDIO_VOLUME = {
    "audio.volume", SK_INT_PCT, 0, g_audio_volume, s_audio_volume, 0,
    "master volume (0-100)"
};
static const settings_entry_t E_AUDIO_MUTED = {
    "audio.muted", SK_BOOL, 0, g_audio_muted, s_audio_muted, 0,
    "master mute (0/1)"
};
static const settings_entry_t E_DISPLAY_BRIGHTNESS = {
    "display.brightness", SK_INT_PCT, 0, g_display_brightness, s_display_brightness, 0,
    "backlight (0-100, 'n/a' if no _BCL)"
};
static const settings_entry_t E_IDLE_DIM = {
    "idle.dim_secs", SK_INT_SECS, 0, g_idle_dim, s_idle_dim, 0,
    "seconds of input idle before dim"
};
static const settings_entry_t E_IDLE_LOCK = {
    "idle.lock_secs", SK_INT_SECS, 0, g_idle_lock, s_idle_lock, 0,
    "seconds of input idle before lock"
};
static const settings_entry_t E_IDLE_BLANK = {
    "idle.blank_secs", SK_INT_SECS, 0, g_idle_blank, s_idle_blank, 0,
    "seconds of input idle before blank"
};
static const settings_entry_t E_LOCK_PIN = {
    "lock.pin", SK_STRING, SETTINGS_FLAG_WRITEONLY, g_lock_pin, s_lock_pin, 0,
    "lock screen PIN (4-16 digits, write-only)"
};
static const settings_entry_t E_LOCK_COLD_BOOT = {
    "lock.cold_boot_required", SK_BOOL, 0, g_lock_cold, s_lock_cold, 0,
    "require PIN at cold boot (0/1)"
};
static const settings_entry_t E_TRASH_DAYS = {
    "trash.auto_empty_days", SK_INT_SECS,
    SETTINGS_FLAG_READONLY | SETTINGS_FLAG_STUB,
    g_trash_auto_empty, s_trash_auto_empty, 0,
    "days before auto-empty (currently fixed at 30)"
};
static const settings_entry_t E_POWER_BTN = {
    "power.button_action", SK_ENUM, 0,
    g_power_btn, s_power_btn, "suspend|shutdown|lock|ask|nothing|wake",
    "power button action"
};
static const settings_entry_t E_POWER_LID_CLOSE = {
    "power.lid_close_action", SK_ENUM, 0,
    g_power_lid_close, s_power_lid_close, "suspend|shutdown|lock|ask|nothing|wake",
    "lid close action"
};
static const settings_entry_t E_POWER_LID_OPEN = {
    "power.lid_open_action", SK_ENUM, 0,
    g_power_lid_open, s_power_lid_open, "suspend|shutdown|lock|ask|nothing|wake",
    "lid open action"
};
static const settings_entry_t E_TIME_TZ = {
    "time.tz_offset_minutes", SK_INT, 0, g_tz, s_tz, 0,
    "timezone offset in minutes east of UTC"
};
static const settings_entry_t E_NET_DHCP = {
    "network.dhcp_enabled", SK_BOOL,
    SETTINGS_FLAG_READONLY | SETTINGS_FLAG_STUB,
    g_net_dhcp, s_net_dhcp, 0,
    "DHCP toggle (no live switch yet)"
};
static const settings_entry_t E_THEME_DARK = {
    "theme.dark_mode", SK_BOOL, 0, g_theme_dark, s_theme_dark, 0,
    "dark mode (0=light, 1=dark/auto)"
};
static const settings_entry_t E_WM_SNAP_DRAG = {
    "wm.snap_drag_threshold", SK_INT_PCT, 0,
    g_wm_snap_drag, s_wm_snap_drag, 0,
    "drag-to-edge snap activation distance (px from edge)"
};
static const settings_entry_t E_WM_SNAP_ANIM = {
    "wm.snap_animation_ms", SK_INT, 0,
    g_wm_snap_anim, s_wm_snap_anim, 0,
    "snap commit spring animation total time (ms)"
};
static const settings_entry_t E_WM_WS_COUNT = {
    "wm.workspace_count", SK_INT, 0,
    g_wm_ws_count, s_wm_ws_count, 0,
    "number of virtual desktops (1..8, default 4)"
};
static const settings_entry_t E_WM_WS_ANIM = {
    "wm.workspace_animation_ms", SK_INT, 0,
    g_wm_ws_anim, s_wm_ws_anim, 0,
    "workspace switch slide animation total time (ms)"
};
static const settings_entry_t E_MDE_SELECT_POLICY = {
    "mde.select_policy", SK_ENUM, 0,
    g_mde_policy, s_mde_policy,
    "first|round_robin|least_loaded|shape_aware",
    "CHAIN_MDE backend selection policy"
};

void settings_register_all(void)
{
    static int done = 0;
    if (done) return;
    done = 1;

    settings_register(&E_AUDIO_VOLUME);
    settings_register(&E_AUDIO_MUTED);
    settings_register(&E_DISPLAY_BRIGHTNESS);
    settings_register(&E_IDLE_DIM);
    settings_register(&E_IDLE_LOCK);
    settings_register(&E_IDLE_BLANK);
    settings_register(&E_LOCK_PIN);
    settings_register(&E_LOCK_COLD_BOOT);
    settings_register(&E_TRASH_DAYS);
    settings_register(&E_POWER_BTN);
    settings_register(&E_POWER_LID_CLOSE);
    settings_register(&E_POWER_LID_OPEN);
    settings_register(&E_TIME_TZ);
    settings_register(&E_NET_DHCP);
    settings_register(&E_THEME_DARK);
    settings_register(&E_MDE_SELECT_POLICY);
    settings_register(&E_WM_SNAP_DRAG);
    settings_register(&E_WM_SNAP_ANIM);
    settings_register(&E_WM_WS_COUNT);
    settings_register(&E_WM_WS_ANIM);

    /* Firewall: enabled + default_inbound + default_outbound. */
    {
        extern void firewall_settings_register(void);
        firewall_settings_register();
    }
}

void settings_print_selftest_line(void)
{
    int total = settings_count();
    int live  = settings_live_count();
    kputs("  Settings .............. ");
    kput_dec((uint64_t)total);
    kputs(" keys registered, ");
    kput_dec((uint64_t)live);
    kputs(" from current modules, chain applies=");
    kput_dec((uint64_t)s_settings_apply_total);
    kputs("\n");
}
