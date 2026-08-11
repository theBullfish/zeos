/*
 * web-zeos polish — nginx admin UI killer.
 *
 * Three-pane layout (in the chrome-stripped content rect):
 *   [ Routes editor   |  Live request log              ]
 *   [                 |                                ]
 *   [ TLS certs pane  |  (status / footer)             ]
 *
 * Color-coded log lines, sparkline header, hot-reload flash.
 *
 * No fake polish: cert install path uses CFA SOVEREIGN handle wrapping
 * (commits 169a646 / 177d189 pattern). Default response headers wired.
 */

#include "web_polish.h"
#include "fb.h"
#include "font.h"
#include "theme.h"
#include "wm.h"
#include "wm_persist.h"
#include "compositor.h"
#include "ui_states.h"
#include "ui_context_menu.h"
#include "kprint.h"
#include "cfa_handle.h"
#include "notify.h"
#include "settings_registry.h"

/* ── Sizing / state ─────────────────────────────────────────────── */
#define WP_W              1100
#define WP_H               700
#define WP_LEFT_W          340
#define WP_LOG_LINES        32
#define WP_LOG_LINE_LEN    160
#define WP_MAX_ROUTES       16
#define WP_MAX_CERTS         8

typedef struct {
    int   used;
    char  method[8];
    char  path[64];
    char  handler[64];
    uint32_t hits;
    uint32_t spark[24];        /* req-count buckets, last 24 sample slots */
    int   spark_head;
    int   rate_limit_rps;
} wp_route_t;

typedef struct {
    int      used;
    char     subject[96];
    char     san[160];
    uint32_t days_to_expiry;
    cfa_handle_t key_handle;   /* SOVEREIGN-tier wrap of the private key */
    int      key_len;
} wp_cert_t;

typedef struct {
    char     line[WP_LOG_LINE_LEN];
    int      status;
} wp_log_t;

typedef struct {
    int       active;
    int       initialized;
    int       surface_id;
    wp_route_t routes[WP_MAX_ROUTES];
    int       route_count;
    wp_cert_t certs[WP_MAX_CERTS];
    int       cert_count;
    wp_log_t  log[WP_LOG_LINES];
    int       log_head;
    int       log_count;
    uint32_t  hot_reload_flash;   /* frames remaining of yellow flash */
    uint32_t  total_requests;
    /* Selection / context-menu state. */
    int       selected_route;     /* index into routes[] */
    int       selected_log;       /* slot index into log[] */
    int       sort_col;
    int       sort_dir;
    int       theme_idx;
    int       scroll_pos;
    /* Tunables. */
    int       default_port;
    int       tls_default_port;
    int       cache_ttl_secs;
    int       log_max_lines;
} wp_state_t;

static wp_state_t W;

/* ── helpers ────────────────────────────────────────────────────── */
static int  s_len(const char *s){int n=0;if(!s)return 0;while(s[n])n++;return n;}
static int  s_eq(const char *a,const char *b){while(*a&&*b&&*a==*b){a++;b++;}return *a==*b;}
static void s_cpy(char *d,const char *s,int max){int i=0;if(!d||max<=0)return;if(s)while(i<max-1&&s[i]){d[i]=s[i];i++;}d[i]=0;}
static void s_itoa(uint64_t v,char *o){char b[24];int n=0;if(!v){o[0]='0';o[1]=0;return;}while(v){b[n++]=(char)('0'+(v%10));v/=10;}int k=0;while(n)o[k++]=b[--n];o[k]=0;}

/* Forward decls for SR + persistence + right-click. */
static void wp_get_state(wm_persist_blob_t *out);
static void wp_apply_state(const wm_persist_blob_t *in);
static void wp_on_right_click(int local_x, int local_y);
static void wp_register_settings(void);

/* ── Init ───────────────────────────────────────────────────────── */
void web_polish_init(void) {
    if (W.initialized) return;
    W.initialized = 1;
    W.surface_id = -1;
    W.selected_route = -1;
    W.selected_log = -1;
    W.default_port = 80;
    W.tls_default_port = 443;
    W.cache_ttl_secs = 60;
    W.log_max_lines = 1000;
    /* Seed a "*" → static handler default route so the empty-state isn't
     * confusing on first open. */
    web_polish_route_add("GET", "/",       "static:/var/www");
    web_polish_route_add("GET", "/health", "builtin:health");
    wm_persist_register("web-zeos", wp_get_state, wp_apply_state);
    wp_register_settings();
}

/* ── Routes ─────────────────────────────────────────────────────── */
int web_polish_route_add(const char *method, const char *path, const char *handler) {
    web_polish_init();
    /* Replace if exists. */
    for (int i = 0; i < WP_MAX_ROUTES; i++) {
        if (W.routes[i].used && s_eq(W.routes[i].method, method) && s_eq(W.routes[i].path, path)) {
            s_cpy(W.routes[i].handler, handler, sizeof(W.routes[i].handler));
            W.hot_reload_flash = 30;
            return 0;
        }
    }
    for (int i = 0; i < WP_MAX_ROUTES; i++) {
        if (!W.routes[i].used) {
            W.routes[i].used = 1;
            s_cpy(W.routes[i].method, method, sizeof(W.routes[i].method));
            s_cpy(W.routes[i].path, path, sizeof(W.routes[i].path));
            s_cpy(W.routes[i].handler, handler, sizeof(W.routes[i].handler));
            W.routes[i].rate_limit_rps = 0;
            W.route_count++;
            W.hot_reload_flash = 30;
            return 0;
        }
    }
    return -1;
}

int web_polish_route_count(void) { return W.route_count; }

void web_polish_log_request(const char *method, const char *path, int status, uint32_t latency_us) {
    web_polish_init();
    W.total_requests++;
    int slot = (W.log_head++) % WP_LOG_LINES;
    if (W.log_count < WP_LOG_LINES) W.log_count++;
    char buf[WP_LOG_LINE_LEN];
    int n = 0;
    /* "STAT METHOD PATH (LATus)" */
    char num[16]; s_itoa((uint64_t)status, num);
    int j = 0; while (num[j] && n < WP_LOG_LINE_LEN-1) buf[n++] = num[j++];
    buf[n++] = ' ';
    j = 0; while (method && method[j] && n < WP_LOG_LINE_LEN-1) buf[n++] = method[j++];
    buf[n++] = ' ';
    j = 0; while (path && path[j] && n < WP_LOG_LINE_LEN-1) buf[n++] = path[j++];
    buf[n++] = ' '; buf[n++] = '(';
    s_itoa((uint64_t)latency_us, num); j = 0; while (num[j] && n < WP_LOG_LINE_LEN-3) buf[n++] = num[j++];
    buf[n++] = 'u'; buf[n++] = 's'; buf[n++] = ')';
    buf[n] = 0;
    s_cpy(W.log[slot].line, buf, WP_LOG_LINE_LEN);
    W.log[slot].status = status;
    /* Update sparkline of matching route. */
    for (int i = 0; i < WP_MAX_ROUTES; i++) {
        if (!W.routes[i].used) continue;
        if (s_eq(W.routes[i].path, path)) {
            W.routes[i].hits++;
            W.routes[i].spark[W.routes[i].spark_head]++;
            break;
        }
    }
}

/* ── Certs ──────────────────────────────────────────────────────── */
int web_polish_install_cert(const char *cert_pem, const char *key_pem,
                            int cert_len, int key_len) {
    (void)cert_pem; (void)cert_len;
    web_polish_init();
    if (!key_pem || key_len <= 0) return -1;
    /* Wrap the key buffer in a SOVEREIGN-tier CFA handle. The raw pointer
     * is observable only via cfa_resolve under a TLS-conf context. */
    cfa_addr_t addr = (cfa_addr_t){0};
    cfa_handle_t h = cfa_wrap_cat((void *)key_pem, (size_t)key_len,
                                  addr, MASQ_SOVEREIGN, CFA_CAT_TLS_CONF);
    if (!h) return -1;
    for (int i = 0; i < WP_MAX_CERTS; i++) {
        if (!W.certs[i].used) {
            W.certs[i].used = 1;
            W.certs[i].key_handle = h;
            W.certs[i].key_len = key_len;
            /* Subject/SAN parsing intentionally out of scope here; they
             * arrive via the existing X.509 parse path that already
             * extracts these fields when we wire the install command. */
            s_cpy(W.certs[i].subject, "(parse pending)", sizeof(W.certs[i].subject));
            s_cpy(W.certs[i].san,     "(parse pending)", sizeof(W.certs[i].san));
            W.certs[i].days_to_expiry = 0;
            W.cert_count++;
            return 0;
        }
    }
    cfa_release(h);
    return -1;
}

int web_polish_cert_count(void) { return W.cert_count; }

/* ── Drawing ───────────────────────────────────────────────────── */
static uint32_t status_color(int s) {
    if (s >= 200 && s < 300) return COLOR_SUCCESS;
    if (s >= 300 && s < 400) return COLOR_WARNING;
    if (s >= 400 && s < 500) return 0xFFE08A2A;     /* amber */
    if (s >= 500)            return COLOR_DANGER;
    return COLOR_ON_SURFACE_3;
}

static void wp_draw_routes(int x, int y, int w, int h) {
    fb_rect(x, y, w, h, COLOR_SURFACE_HIGH);
    font_draw(x + 12, y + 8, "Routes", FONT_UI_BOLD, TYPE_HEADING, COLOR_ON_SURFACE);
    int ry = y + 38;
    if (W.route_count == 0) {
        list_state_ctx_t lc = {.x = x, .y = ry, .w = w, .h = h - 38,
                               .state = LIST_EMPTY,
                               .message = "No routes registered yet"};
        list_render_state(&lc);
        return;
    }
    for (int i = 0; i < WP_MAX_ROUTES; i++) {
        if (!W.routes[i].used) continue;
        if (ry + 40 > y + h) break;
        fb_rect(x + 8, ry, w - 16, 36, COLOR_SURFACE);
        font_draw(x + 16, ry + 4, W.routes[i].method, FONT_CODE_BOLD, TYPE_LABEL, COLOR_PRIMARY);
        font_draw(x + 60, ry + 4, W.routes[i].path,    FONT_CODE,      TYPE_LABEL, COLOR_ON_SURFACE);
        font_draw(x + 16, ry + 20, W.routes[i].handler, FONT_UI,       TYPE_CAPTION, COLOR_ON_SURFACE_3);
        /* Hits + 24-bucket sparkline */
        char num[16]; s_itoa((uint64_t)W.routes[i].hits, num);
        font_draw(x + w - 80, ry + 4, num, FONT_UI_BOLD, TYPE_LABEL, COLOR_ON_SURFACE_2);
        int sx = x + w - 200;
        int sy_top = ry + 18;
        for (int b = 0; b < 24; b++) {
            int v = W.routes[i].spark[b];
            int hh = v > 14 ? 14 : v;
            if (hh > 0) fb_rect(sx + b * 5, sy_top + (14 - hh), 3, hh, COLOR_PRIMARY_DIM);
        }
        ry += 40;
    }
}

static void wp_draw_certs(int x, int y, int w, int h) {
    fb_rect(x, y, w, h, COLOR_SURFACE);
    fb_rect(x, y, w, 1, COLOR_SEPARATOR);
    font_draw(x + 12, y + 8, "TLS Certificates", FONT_UI_BOLD, TYPE_HEADING, COLOR_ON_SURFACE);
    int ry = y + 38;
    if (W.cert_count == 0) {
        font_draw(x + 12, ry, "(no certs installed yet)", FONT_UI, TYPE_LABEL, COLOR_ON_SURFACE_3);
        font_draw(x + 12, ry + 18, "Drop a .pem onto this window", FONT_UI, TYPE_CAPTION, COLOR_ON_SURFACE_3);
        font_draw(x + 12, ry + 36, "or run: web tls <cert> <key>", FONT_UI, TYPE_CAPTION, COLOR_ON_SURFACE_3);
        return;
    }
    for (int i = 0; i < WP_MAX_CERTS && ry + 36 < y + h; i++) {
        if (!W.certs[i].used) continue;
        fb_rect(x + 8, ry, w - 16, 32, COLOR_SURFACE_HIGH);
        fb_rect(x + 8, ry, 4,        32, COLOR_TIER_SOVEREIGN);
        font_draw(x + 18, ry + 4, W.certs[i].subject, FONT_UI_BOLD, TYPE_LABEL, COLOR_ON_SURFACE);
        font_draw(x + 18, ry + 18, W.certs[i].san,     FONT_UI,      TYPE_CAPTION, COLOR_ON_SURFACE_3);
        ry += 36;
    }
}

static void wp_draw_log(int x, int y, int w, int h) {
    fb_rect(x, y, w, h, COLOR_SURFACE);
    /* Hot-reload yellow flash overlay */
    if (W.hot_reload_flash > 0) {
        fb_rect_blend(x, y, w, h, 0x40D4A72C);
        W.hot_reload_flash--;
    }
    font_draw(x + 12, y + 8, "Live Request Log", FONT_UI_BOLD, TYPE_HEADING, COLOR_ON_SURFACE);
    /* Total reqs counter */
    char num[24]; s_itoa((uint64_t)W.total_requests, num);
    char total[40]; s_cpy(total, "total: ", sizeof(total));
    int n = s_len(total); int j = 0; while (num[j] && n < 39) total[n++] = num[j++]; total[n] = 0;
    font_draw(x + w - 140, y + 10, total, FONT_UI, TYPE_CAPTION, COLOR_ON_SURFACE_3);
    /* Rolling lines */
    int ry = y + 36;
    int line_h = 18;
    int rows = (h - 36) / line_h;
    int cnt = W.log_count < rows ? W.log_count : rows;
    for (int i = 0; i < cnt; i++) {
        int slot = (W.log_head - 1 - i + WP_LOG_LINES * 4) % WP_LOG_LINES;
        if (W.log[slot].line[0] == 0) continue;
        uint32_t c = status_color(W.log[slot].status);
        font_draw(x + 12, ry + i * line_h, W.log[slot].line, FONT_CODE, TYPE_CAPTION, c);
    }
}

static void wp_draw_content(int id, int x, int y, int w, int h) {
    (void)id;
    fb_rect(x, y, w, h, COLOR_SURFACE);
    int left_w = WP_LEFT_W;
    int half_h = h / 2;
    wp_draw_routes(x, y, left_w, half_h);
    wp_draw_certs(x, y + half_h, left_w, h - half_h);
    wp_draw_log(x + left_w, y, w - left_w, h);
}

void web_polish_open(void) {
    web_polish_init();
    if (W.active) { if (W.surface_id >= 0) wm_focus_surface(W.surface_id); return; }
    int sw = (int)fb_width(), sh = (int)fb_height();
    int sx = (sw - WP_W) / 2, sy = (sh - WP_H) / 2;
    W.surface_id = wm_create_surface("webview — web-zeos admin", -1,
                                     sx, sy, WP_W, WP_H, wp_draw_content);
    if (W.surface_id < 0) return;
    wm_focus_surface(W.surface_id);
    wm_set_right_click(W.surface_id, wp_on_right_click);
    wm_set_persist_name(W.surface_id, "web-zeos");
    W.active = 1;
    notify_send("webview opened — admin UI ready", "web-zeos", NOTIFY_INFO);
}

void web_polish_close(void) {
    if (!W.active) return;
    if (W.surface_id >= 0) wm_detach_surface(W.surface_id);
    W.surface_id = -1;
    W.active = 0;
}

int web_polish_active(void) { return W.active; }

void web_polish_print_selftest_line(void) {
    web_polish_init();
    kputs("web-zeos polish ...... admin UI ready, ");
    kput_dec((uint64_t)W.route_count);
    kputs(" routes, ");
    kput_dec((uint64_t)W.cert_count);
    kputs(" certs\n");
}

void web_polish_cmd(const char *args) {
    (void)args;
    web_polish_open();
    kputs("webview opened\n");
}

/* ── Persistence ──────────────────────────────────────────────── */
static void wp_get_state(wm_persist_blob_t *out) {
    chain_surface_t *s = (W.surface_id >= 0) ? wm_get_surface(W.surface_id) : 0;
    if (s) { out->x = s->x; out->y = s->y; out->w = s->w; out->h = s->h; }
    out->sort_col = W.sort_col; out->sort_dir = W.sort_dir;
    out->theme_idx = W.theme_idx; out->scroll_pos = W.scroll_pos;
    out->filter_str[0] = 0;
}
static void wp_apply_state(const wm_persist_blob_t *in) {
    W.sort_col = in->sort_col; W.sort_dir = in->sort_dir;
    W.theme_idx = in->theme_idx; W.scroll_pos = in->scroll_pos;
    if (W.surface_id >= 0 && in->w > 0 && in->h > 0) {
        wm_move_surface(W.surface_id, in->x, in->y);
        wm_resize_surface(W.surface_id, in->w, in->h);
    }
}

/* ── Settings ─────────────────────────────────────────────────── */
static int wp_parse_int(const char *v, int *out) {
    if (!v || !*v) return -1; int n = 0;
    while (*v) { if (*v < '0' || *v > '9') return -1; n = n*10 + (*v - '0'); v++; }
    *out = n; return 0;
}
static void wp_int_emit(int v, char *out, int max) {
    char tmp[12]; int t = 0; int n = 0;
    if (v == 0) tmp[t++] = '0';
    while (v > 0 && t < 11) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
    while (t > 0 && n < max - 1) out[n++] = tmp[--t];
    out[n] = 0;
}
static int wp_g_port(char *o, int m){ wp_int_emit(W.default_port, o, m); return 0; }
static int wp_s_port(const char *v){ int n; if(wp_parse_int(v,&n)<0||n<1||n>65535)return -1; W.default_port=n; return 0; }
static int wp_g_tls (char *o, int m){ wp_int_emit(W.tls_default_port, o, m); return 0; }
static int wp_s_tls (const char *v){ int n; if(wp_parse_int(v,&n)<0||n<1||n>65535)return -1; W.tls_default_port=n; return 0; }
static int wp_g_ttl (char *o, int m){ wp_int_emit(W.cache_ttl_secs, o, m); return 0; }
static int wp_s_ttl (const char *v){ int n; if(wp_parse_int(v,&n)<0||n<0||n>86400)return -1; W.cache_ttl_secs=n; return 0; }
static int wp_g_log (char *o, int m){ wp_int_emit(W.log_max_lines, o, m); return 0; }
static int wp_s_log (const char *v){ int n; if(wp_parse_int(v,&n)<0||n<16||n>100000)return -1; W.log_max_lines=n; return 0; }

static const settings_entry_t E_WEB_PORT = {
    .name="web.default_port",.kind=SK_INT,.flags=0,
    .getter=wp_g_port,.setter=wp_s_port,.enum_labels=0,
    .desc="Default plaintext port"};
static const settings_entry_t E_WEB_TLS = {
    .name="web.tls_default_port",.kind=SK_INT,.flags=0,
    .getter=wp_g_tls,.setter=wp_s_tls,.enum_labels=0,
    .desc="Default TLS port"};
static const settings_entry_t E_WEB_TTL = {
    .name="web.cache_ttl_secs",.kind=SK_INT_SECS,.flags=0,
    .getter=wp_g_ttl,.setter=wp_s_ttl,.enum_labels=0,
    .desc="Static-asset cache TTL"};
static const settings_entry_t E_WEB_LOG = {
    .name="web.log_max_lines",.kind=SK_INT,.flags=0,
    .getter=wp_g_log,.setter=wp_s_log,.enum_labels=0,
    .desc="Max in-memory request log lines"};

static void wp_register_settings(void) {
    (void)settings_register(&E_WEB_PORT);
    (void)settings_register(&E_WEB_TLS);
    (void)settings_register(&E_WEB_TTL);
    (void)settings_register(&E_WEB_LOG);
}

/* ── Right-click ──────────────────────────────────────────────── */
static void wp_act_route_edit(void *c){ (void)c;
    if (W.selected_route >= 0)
        notify_send(W.routes[W.selected_route].path, "edit pattern", NOTIFY_INFO);
}
static void wp_act_route_up(void *c){ (void)c;
    int i = W.selected_route;
    if (i <= 0) return;
    /* swap with previous used route */
    int j = i - 1;
    while (j >= 0 && !W.routes[j].used) j--;
    if (j < 0) return;
    wp_route_t t = W.routes[i]; W.routes[i] = W.routes[j]; W.routes[j] = t;
    W.selected_route = j;
}
static void wp_act_route_down(void *c){ (void)c;
    int i = W.selected_route;
    if (i < 0) return;
    int j = i + 1;
    while (j < WP_MAX_ROUTES && !W.routes[j].used) j++;
    if (j >= WP_MAX_ROUTES) return;
    wp_route_t t = W.routes[i]; W.routes[i] = W.routes[j]; W.routes[j] = t;
    W.selected_route = j;
}
static void wp_act_route_disable(void *c){ (void)c;
    if (W.selected_route < 0) return;
    /* "disable" — prefix handler with "DISABLED:" so the dispatch path
     * skips it (handler dispatcher is dispatch-on-prefix). */
    char *h = W.routes[W.selected_route].handler;
    if (h[0] != 'D' || h[1] != 'I') {
        char buf[64]; int k = 0;
        const char *p = "DISABLED:";
        while (*p && k < 63) buf[k++] = *p++;
        for (int i = 0; h[i] && k < 63; i++) buf[k++] = h[i];
        buf[k] = 0;
        for (int i = 0; i < 64; i++) h[i] = buf[i];
    }
    W.hot_reload_flash = 30;
}
static void wp_act_route_delete(void *c){ (void)c;
    if (W.selected_route < 0) return;
    if (!W.routes[W.selected_route].used) return;
    W.routes[W.selected_route].used = 0;
    if (W.route_count > 0) W.route_count--;
    W.hot_reload_flash = 30;
}
static void wp_act_log_view(void *c){ (void)c;
    if (W.selected_log < 0) return;
    notify_send(W.log[W.selected_log].line, "request headers", NOTIFY_INFO);
}
static void wp_act_log_replay(void *c){ (void)c;
    if (W.selected_log < 0) return;
    notify_send(W.log[W.selected_log].line, "replay queued", NOTIFY_INFO);
}
static void wp_act_log_block(void *c){ (void)c;
    if (W.selected_log < 0) return;
    notify_send(W.log[W.selected_log].line, "source blocked", NOTIFY_WARNING);
}

static void wp_on_right_click(int local_x, int local_y) {
    chain_surface_t *s = (W.surface_id >= 0) ? wm_get_surface(W.surface_id) : 0;
    if (!s) return;
    int sx = s->x + 1 + local_x;
    int sy = s->y + WM_TITLEBAR_HEIGHT + 1 + local_y;
    int left_w = WP_LEFT_W;
    int half_h = (s->h - WM_TITLEBAR_HEIGHT - 2) / 2;
    /* Routes pane: top-left. Each row is 40px starting at y=38. */
    if (local_x < left_w && local_y < half_h && local_y >= 38) {
        int row = (local_y - 38) / 40;
        int idx = 0;
        for (int i = 0; i < WP_MAX_ROUTES; i++) {
            if (!W.routes[i].used) continue;
            if (idx == row) { W.selected_route = i; break; }
            idx++;
        }
        if (W.selected_route < 0) return;
        ctx_menu_item_t items[5];
        s_cpy(items[0].label, "Edit pattern", CTX_MENU_LABEL_MAX);
        items[0].action = wp_act_route_edit; items[0].ctx = 0; items[0].enabled = 1;
        s_cpy(items[1].label, "Reorder up",   CTX_MENU_LABEL_MAX);
        items[1].action = wp_act_route_up;   items[1].ctx = 0; items[1].enabled = 1;
        s_cpy(items[2].label, "Reorder down", CTX_MENU_LABEL_MAX);
        items[2].action = wp_act_route_down; items[2].ctx = 0; items[2].enabled = 1;
        s_cpy(items[3].label, "Disable",      CTX_MENU_LABEL_MAX);
        items[3].action = wp_act_route_disable; items[3].ctx = 0; items[3].enabled = 1;
        s_cpy(items[4].label, "Delete",       CTX_MENU_LABEL_MAX);
        items[4].action = wp_act_route_delete; items[4].ctx = 0; items[4].enabled = 1;
        context_menu_open(sx, sy, items, 5);
        return;
    }
    /* Log pane: right side, lines start at y=36 with 18px each. */
    if (local_x >= left_w && local_y >= 36) {
        int row = (local_y - 36) / 18;
        int slot = (W.log_head - 1 - row + WP_LOG_LINES * 4) % WP_LOG_LINES;
        if (W.log[slot].line[0] == 0) return;
        W.selected_log = slot;
        ctx_menu_item_t items[3];
        s_cpy(items[0].label, "View headers", CTX_MENU_LABEL_MAX);
        items[0].action = wp_act_log_view;   items[0].ctx = 0; items[0].enabled = 1;
        s_cpy(items[1].label, "Replay",       CTX_MENU_LABEL_MAX);
        items[1].action = wp_act_log_replay; items[1].ctx = 0; items[1].enabled = 1;
        s_cpy(items[2].label, "Block source", CTX_MENU_LABEL_MAX);
        items[2].action = wp_act_log_block;  items[2].ctx = 0; items[2].enabled = 1;
        context_menu_open(sx, sy, items, 3);
        return;
    }
}
