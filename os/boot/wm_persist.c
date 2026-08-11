/*
 * WM per-app per-CFA-context persistence — implementation.
 *
 * State path: /wm/<ctx_id>/<app_name>. CRC32 (poly 0xEDB88320) over
 * the blob with crc32 field zeroed at compute time. Mismatch on read
 * = treat as no state, return -1 from restore.
 */

#include "wm_persist.h"
#include "vault.h"
#include "identity.h"
#include "timeofday.h"

/* ── String helpers (no libc) ─────────────────────────────────── */
static int wp_streq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static void wp_strcpy(char *d, const char *s, int max) {
    int i = 0; if (!d || max <= 0) return;
    if (s) while (i < max - 1 && s[i]) { d[i] = s[i]; i++; }
    d[i] = 0;
}
static void wp_itoa(int v, char *o, int max) {
    if (max <= 0) return;
    char tmp[12]; int n = 0;
    if (v < 0) { *o++ = '-'; max--; v = -v; }
    if (v == 0) { tmp[n++] = '0'; }
    while (v > 0 && n < 11) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    int k = 0; while (n > 0 && k < max - 1) o[k++] = tmp[--n]; o[k] = 0;
}

/* ── CRC32 (IEEE / 0xEDB88320) ────────────────────────────────── */
static uint32_t wp_crc32(const void *data, int len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (int i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = -(int32_t)(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

/* ── Registry ─────────────────────────────────────────────────── */
typedef struct {
    int                   used;
    char                  name[WM_PERSIST_NAME_MAX];
    wm_persist_get_fn     get;
    wm_persist_apply_fn   apply;
} wp_entry_t;

static wp_entry_t       g_apps[WM_PERSIST_MAX_APPS];
static uint32_t         g_total_saves;
static uint32_t         g_total_restores;
static uint32_t         g_last_tick_save_ms;

#define WP_AUTOSAVE_INTERVAL_MS 60000u

static int wp_find(const char *app_name) {
    for (int i = 0; i < WM_PERSIST_MAX_APPS; i++) {
        if (g_apps[i].used && wp_streq(g_apps[i].name, app_name)) return i;
    }
    return -1;
}

int wm_persist_register(const char *app_name,
                        wm_persist_get_fn get,
                        wm_persist_apply_fn apply) {
    if (!app_name || !*app_name || !get || !apply) return -1;
    int slot = wp_find(app_name);
    if (slot < 0) {
        for (int i = 0; i < WM_PERSIST_MAX_APPS; i++) {
            if (!g_apps[i].used) { slot = i; break; }
        }
    }
    if (slot < 0) return -1;
    g_apps[slot].used = 1;
    wp_strcpy(g_apps[slot].name, app_name, WM_PERSIST_NAME_MAX);
    g_apps[slot].get = get;
    g_apps[slot].apply = apply;
    return 0;
}

/* Build "/wm/<ctx>/<app>" path. */
static void wp_path(int ctx_id, const char *app_name, char *out, int max) {
    if (max <= 0) return;
    int p = 0;
    const char *prefix = "/wm/";
    while (prefix[p] && p < max - 1) { out[p] = prefix[p]; p++; }
    /* ctx id */
    char num[12]; wp_itoa(ctx_id, num, sizeof(num));
    int j = 0; while (num[j] && p < max - 1) out[p++] = num[j++];
    if (p < max - 1) out[p++] = '/';
    /* app name */
    j = 0; while (app_name[j] && p < max - 1) out[p++] = app_name[j++];
    out[p] = 0;
}

static int wp_active_ctx(void) {
    int c = identity_context_active();
    return c > 0 ? c : 1;
}

/* ── Save / restore ───────────────────────────────────────────── */
int wm_persist_save(const char *app_name) {
    int slot = wp_find(app_name);
    if (slot < 0) return -1;
    wm_persist_blob_t blob;
    /* Zero whole blob, then let the app fill in. */
    uint8_t *bp = (uint8_t *)&blob;
    for (uint32_t i = 0; i < sizeof(blob); i++) bp[i] = 0;
    blob.version = WM_PERSIST_BLOB_VERSION;
    blob.crc32 = 0;
    g_apps[slot].get(&blob);
    /* Re-stamp version (in case the app zeroed it) and compute CRC. */
    blob.version = WM_PERSIST_BLOB_VERSION;
    blob.crc32 = 0;
    blob.crc32 = wp_crc32(&blob, (int)sizeof(blob));
    char path[96];
    wp_path(wp_active_ctx(), g_apps[slot].name, path, sizeof(path));
    int wrote = vault_write(path, &blob, (uint32_t)sizeof(blob));
    if (wrote < 0) return -1;
    g_total_saves++;
    return 0;
}

void wm_persist_save_all(void) {
    for (int i = 0; i < WM_PERSIST_MAX_APPS; i++) {
        if (!g_apps[i].used) continue;
        (void)wm_persist_save(g_apps[i].name);
    }
}

int wm_persist_restore(const char *app_name) {
    int slot = wp_find(app_name);
    if (slot < 0) return -1;
    char path[96];
    wp_path(wp_active_ctx(), g_apps[slot].name, path, sizeof(path));
    wm_persist_blob_t blob;
    int got = vault_read(path, &blob, (uint32_t)sizeof(blob));
    if (got < (int)sizeof(blob)) return -1;
    if (blob.version != WM_PERSIST_BLOB_VERSION) return -1;
    /* Verify CRC. */
    uint32_t saved_crc = blob.crc32;
    blob.crc32 = 0;
    uint32_t calc = wp_crc32(&blob, (int)sizeof(blob));
    if (calc != saved_crc) return -1;
    blob.crc32 = saved_crc;
    g_apps[slot].apply(&blob);
    g_total_restores++;
    return 0;
}

void wm_persist_tick(void) {
    uint32_t now = tod_now_millis();
    if (g_last_tick_save_ms == 0) {
        g_last_tick_save_ms = now ? now : 1;
        return;
    }
    /* tod_now_millis can wrap or return 0 before init; treat 0 as "skip". */
    if (now == 0) return;
    uint32_t dt = now - g_last_tick_save_ms;
    if (dt >= WP_AUTOSAVE_INTERVAL_MS) {
        wm_persist_save_all();
        g_last_tick_save_ms = now;
    }
}

uint32_t wm_persist_app_count(void) {
    uint32_t n = 0;
    for (int i = 0; i < WM_PERSIST_MAX_APPS; i++)
        if (g_apps[i].used) n++;
    return n;
}
uint32_t wm_persist_total_saves(void)    { return g_total_saves; }
uint32_t wm_persist_total_restores(void) { return g_total_restores; }
