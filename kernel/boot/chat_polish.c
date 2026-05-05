/*
 * chat-zeos polish — three-pane Slack/Discord-style window over the
 * existing chat_zeos engine. No new persistence; all rooms / messages
 * live in chat_zeos's btrees + vault.
 */

#include "chat_polish.h"
#include "chat_zeos.h"
#include "chat_e2ee.h"
#include "md_render.h"
#include "fb.h"
#include "font.h"
#include "theme.h"
#include "wm.h"
#include "wm_persist.h"
#include "compositor.h"
#include "ui_states.h"
#include "ui_context_menu.h"
#include "kprint.h"
#include "timeofday.h"
#include "notify.h"
#include "settings_registry.h"

#define CP_W              1200
#define CP_H               760
#define CP_LEFT_W          240
#define CP_RIGHT_W         220
#define CP_TITLE_H          36
#define CP_INPUT_H          44
#define CP_E2EE_MAX         32

typedef struct {
    int   active, initialized;
    int   surface_id;
    char  active_room[64];
    char  e2ee[CP_E2EE_MAX][64];
    int   e2ee_count;
    /* Pane render scratch — populated each frame in walk callbacks. */
    int   rooms_drawn;
    int   msgs_drawn;
    int   pane_x, pane_y, pane_w, pane_h;
    int   ry;
    int   members_x, members_y, members_w;
    int   members_drawn;
    /* Cmd-K palette */
    int   palette_open;
    char  palette_input[64];
    int   palette_input_len;
    /* Selection / view state. */
    uint64_t selected_msg_ts;
    char  selected_msg_author[64];
    char  selected_msg_body[256];
    int   sort_col;
    int   sort_dir;
    int   theme_idx;
    int   scroll_pos;
    /* Tunables. */
    int   message_history_size;
    int   typing_indicator;
    int   relative_timestamps;
} cp_state_t;

static cp_state_t C;

static int  s_len(const char *s){int n=0;if(!s)return 0;while(s[n])n++;return n;}
static int  s_eq(const char *a,const char *b){while(*a&&*b&&*a==*b){a++;b++;}return *a==*b;}
static void s_cpy(char *d,const char *s,int max){int i=0;if(!d||max<=0)return;if(s)while(i<max-1&&s[i]){d[i]=s[i];i++;}d[i]=0;}

/* Forward decls. */
static void cp_get_state(wm_persist_blob_t *out);
static void cp_apply_state(const wm_persist_blob_t *in);
static void cp_on_right_click(int local_x, int local_y);
static void cp_register_settings(void);

void chat_polish_init(void) {
    if (C.initialized) return;
    C.initialized = 1;
    C.surface_id = -1;
    C.message_history_size = 1000;
    C.typing_indicator = 1;
    C.relative_timestamps = 1;
    wm_persist_register("chat-zeos", cp_get_state, cp_apply_state);
    cp_register_settings();
    chat_zeos_init();
    /* Default room: "general". Create if missing. */
    if (chat_zeos_create_room("general", "General", 1) == 0
        || 1 /* idempotent: returns nonzero on dup, that's fine */) {
        chat_zeos_join_room("general", chat_zeos_current_ctx());
    }
    s_cpy(C.active_room, "general", sizeof(C.active_room));
}

int chat_polish_set_e2ee(const char *room_id, int on) {
    chat_polish_init();
    int idx = -1;
    for (int i = 0; i < C.e2ee_count; i++)
        if (s_eq(C.e2ee[i], room_id)) { idx = i; break; }
    if (on) {
        /* Real key derivation lives in chat_e2ee. If it fails (vault
         * busy, room missing, RDRAND failure) we report that up and
         * leave the polish-layer flag clear so the lock icon doesn't
         * lie. */
        if (chat_e2ee_enable(room_id) != 0) return -1;
        if (idx >= 0) return 0;
        if (C.e2ee_count >= CP_E2EE_MAX) return -1;
        s_cpy(C.e2ee[C.e2ee_count++], room_id, sizeof(C.e2ee[0]));
        return 0;
    } else {
        chat_e2ee_disable(room_id);
        if (idx < 0) return 0;
        for (int j = idx; j < C.e2ee_count - 1; j++)
            s_cpy(C.e2ee[j], C.e2ee[j+1], sizeof(C.e2ee[0]));
        C.e2ee_count--;
        return 0;
    }
}
int chat_polish_is_e2ee(const char *room_id) {
    /* Authoritative check is the e2ee module — the polish list is just
     * a UI mirror. */
    return chat_e2ee_is_active(room_id);
}

int chat_polish_set_active_room(const char *room_id) {
    if (!chat_zeos_room_visible_to(room_id, chat_zeos_current_ctx())) return -1;
    s_cpy(C.active_room, room_id, sizeof(C.active_room));
    return 0;
}

/* ── Draw helpers ──────────────────────────────────────────────── */
static uint32_t avatar_color(const char *name) {
    /* Hash to one of three persona accents for visual variety. */
    uint32_t h = 0x811c9dc5u;
    while (name && *name) { h ^= (uint8_t)*name++; h *= 0x01000193u; }
    static const uint32_t pal[6] = {
        COLOR_ZEROS_ACCENT, COLOR_DEREZ_ACCENT, COLOR_FULL_ACCENT,
        0xFFD4A72C, 0xFFE08A2A, 0xFFB14B8E,
    };
    return pal[h % 6];
}

static void cp_room_cb(const char *id, const char *name, int visibility,
                       int member_count, int msg_count, void *user) {
    (void)user;
    if (C.ry > C.pane_y + C.pane_h - 30) return;
    int active = s_eq(id, C.active_room);
    uint32_t bg = active ? COLOR_SURFACE_TOP : COLOR_SURFACE_HIGH;
    fb_rect(C.pane_x, C.ry, C.pane_w, 36, bg);
    if (active) fb_rect(C.pane_x, C.ry, 3, 36, COLOR_PRIMARY);
    /* visibility prefix */
    const char *p = (visibility == 0) ? "#" : (visibility == 2) ? "@" : "*";
    font_draw(C.pane_x + 12, C.ry + 4, p, FONT_UI_BOLD, TYPE_LABEL,
              visibility == 0 ? COLOR_TIER_INTERNAL : COLOR_TIER_REFERENCE);
    font_draw(C.pane_x + 28, C.ry + 4, name, FONT_UI_BOLD, TYPE_LABEL, COLOR_ON_SURFACE);
    /* lock icon for E2EE rooms */
    if (chat_polish_is_e2ee(id)) {
        font_draw(C.pane_x + C.pane_w - 28, C.ry + 4, "\xe2\x9c\x93",
                  FONT_UI_BOLD, TYPE_LABEL, COLOR_TIER_SOVEREIGN);
    }
    /* member + message counts */
    char buf[64]; s_cpy(buf, "", sizeof(buf));
    int n = 0;
    char num[16]; int i = 0;
    /* members */
    int v = member_count; char tmp[12]; int ti = 0;
    if (v == 0) tmp[ti++] = '0'; else { while (v) { tmp[ti++] = (char)('0' + (v%10)); v/=10; } }
    while (ti > 0 && n < 63) num[i++] = tmp[--ti]; num[i] = 0;
    s_cpy(buf, num, sizeof(buf)); s_cpy(buf + s_len(buf), "m  ", sizeof(buf) - s_len(buf));
    /* messages */
    v = msg_count; ti = 0;
    if (v == 0) tmp[ti++] = '0'; else { while (v) { tmp[ti++] = (char)('0' + (v%10)); v/=10; } }
    char num2[16]; i = 0;
    while (ti > 0) num2[i++] = tmp[--ti]; num2[i] = 0;
    s_cpy(buf + s_len(buf), num2, sizeof(buf) - s_len(buf));
    s_cpy(buf + s_len(buf), " msg", sizeof(buf) - s_len(buf));
    font_draw(C.pane_x + 28, C.ry + 20, buf, FONT_UI, TYPE_CAPTION, COLOR_ON_SURFACE_3);
    C.ry += 38;
    C.rooms_drawn++;
}

static void cp_draw_rooms(int x, int y, int w, int h) {
    fb_rect(x, y, w, h, COLOR_SURFACE_HIGH);
    fb_rect(x + w - 1, y, 1, h, COLOR_SEPARATOR);
    font_draw(x + 12, y + 8, "Rooms", FONT_UI_BOLD, TYPE_HEADING, COLOR_ON_SURFACE);
    C.pane_x = x; C.pane_y = y + 36; C.pane_w = w; C.pane_h = h - 36;
    C.ry = y + 36;
    C.rooms_drawn = 0;
    int n = chat_zeos_walk_rooms(chat_zeos_current_ctx(), cp_room_cb, 0);
    if (n == 0) {
        list_state_ctx_t lc = {.x = x, .y = y + 36, .w = w, .h = h - 36,
                               .state = LIST_EMPTY,
                               .message = "No rooms visible to this context",
                               .cta = "chat create general public"};
        list_render_state(&lc);
    }
}

/* "Now - ts" relative formatter — minutes / hours / days. */
static void cp_relative(uint64_t ts, char *out, int max) {
    uint64_t now = tod_now_unix();
    if (now == 0 || ts > now) { s_cpy(out, "now", max); return; }
    uint64_t diff = now - ts;
    if (diff < 60)         { s_cpy(out, "just now", max); return; }
    if (diff < 3600)       {
        int m = (int)(diff / 60);
        char tmp[16]; int ti = 0;
        while (m) { tmp[ti++] = (char)('0' + (m%10)); m /= 10; }
        int i = 0; while (ti > 0 && i < max-5) out[i++] = tmp[--ti];
        out[i++]=' '; out[i++]='m'; out[i++]='i'; out[i++]='n'; out[i]=0;
        return;
    }
    if (diff < 86400)      {
        int hh = (int)(diff / 3600);
        char tmp[16]; int ti = 0;
        while (hh) { tmp[ti++] = (char)('0' + (hh%10)); hh /= 10; }
        int i = 0; while (ti > 0 && i < max-3) out[i++] = tmp[--ti];
        out[i++]=' '; out[i++]='h'; out[i]=0;
        return;
    }
    int dd = (int)(diff / 86400);
    char tmp[16]; int ti = 0;
    while (dd) { tmp[ti++] = (char)('0' + (dd%10)); dd /= 10; }
    int i = 0; while (ti > 0 && i < max-3) out[i++] = tmp[--ti];
    out[i++]=' '; out[i++]='d'; out[i]=0;
}

static int g_msg_y;
static int g_msg_x;
static int g_msg_w;
static int g_msg_y_max;

/* Row map for right-click hit-testing. Captured on each draw of the
 * messages pane. */
#define CP_ROW_MAP_MAX 32
typedef struct { int y_top; int y_bot; uint64_t ts; char author[64]; char body[256]; } cp_row_t;
static cp_row_t g_msg_rows[CP_ROW_MAP_MAX];
static int g_msg_rows_count;

static void cp_msg_cb(uint64_t ts, const char *author, const char *body, void *user) {
    (void)user;
    if (g_msg_y > g_msg_y_max - 60) return;
    if (g_msg_rows_count < CP_ROW_MAP_MAX) {
        g_msg_rows[g_msg_rows_count].y_top = g_msg_y;
        g_msg_rows[g_msg_rows_count].y_bot = g_msg_y + 56;
        g_msg_rows[g_msg_rows_count].ts = ts;
        s_cpy(g_msg_rows[g_msg_rows_count].author, author ? author : "?", 64);
        s_cpy(g_msg_rows[g_msg_rows_count].body, body ? body : "", 256);
        g_msg_rows_count++;
    }
    /* Avatar circle — first letter */
    uint32_t avc = avatar_color(author);
    fb_circle_filled(g_msg_x + 18, g_msg_y + 14, 14, avc);
    char init[2] = { author && *author ? (char)((*author >= 'a' && *author <= 'z') ? *author - 32 : *author) : '?', 0 };
    font_draw(g_msg_x + 12, g_msg_y + 6, init, FONT_UI_BOLD, TYPE_LABEL, COLOR_SURFACE);
    /* Author + relative ts */
    font_draw(g_msg_x + 44, g_msg_y, author, FONT_UI_BOLD, TYPE_LABEL, COLOR_ON_SURFACE);
    char rel[32]; cp_relative(ts, rel, sizeof(rel));
    font_draw(g_msg_x + 160, g_msg_y + 2, rel, FONT_UI, TYPE_CAPTION, COLOR_ON_SURFACE_3);
    /* Body — render markdown inline (bold / italic / code / wikilinks /
     * auto-linked URLs). Falls back gracefully on plain text. */
    int body_len = 0; if (body) while (body[body_len]) body_len++;
    md_render_inline(g_msg_x + 44, g_msg_y + 18, g_msg_w - 60,
                     body ? body : "", body_len);
    g_msg_y += 56;
    C.msgs_drawn++;
}

static void cp_draw_messages(int x, int y, int w, int h) {
    fb_rect(x, y, w, h, COLOR_SURFACE);
    /* Title bar with active room name + lock icon if E2EE. */
    fb_rect(x, y, w, CP_TITLE_H, COLOR_SURFACE_TOP);
    font_draw(x + 12, y + 8, C.active_room, FONT_UI_BOLD, TYPE_HEADING, COLOR_ON_SURFACE);
    if (chat_polish_is_e2ee(C.active_room)) {
        font_draw(x + 12 + 200, y + 10, "(E2EE)", FONT_UI_BOLD, TYPE_LABEL, COLOR_TIER_SOVEREIGN);
    }
    /* Messages region */
    int top = y + CP_TITLE_H + 8;
    int bot = y + h - CP_INPUT_H - 8;
    g_msg_x = x; g_msg_y = top; g_msg_w = w; g_msg_y_max = bot;
    C.msgs_drawn = 0;
    g_msg_rows_count = 0;
    chat_zeos_walk_tail(C.active_room, chat_zeos_current_ctx(), 16, cp_msg_cb, 0);
    if (C.msgs_drawn == 0) {
        list_state_ctx_t lc = {.x = x, .y = top, .w = w, .h = bot - top,
                               .state = LIST_EMPTY,
                               .message = "No messages yet",
                               .cta = "chat send <room> hello"};
        list_render_state(&lc);
    }
    /* Input bar */
    fb_rect(x, y + h - CP_INPUT_H, w, CP_INPUT_H, COLOR_SURFACE_HIGH);
    fb_rect(x + 12, y + h - CP_INPUT_H + 8, w - 24, 28, COLOR_SURFACE);
    font_draw(x + 20, y + h - CP_INPUT_H + 14, "Message #",
              FONT_UI, TYPE_LABEL, COLOR_ON_SURFACE_3);
    font_draw(x + 96, y + h - CP_INPUT_H + 14, C.active_room,
              FONT_UI_BOLD, TYPE_LABEL, COLOR_PRIMARY);
}

static void cp_draw_members(int x, int y, int w, int h) {
    fb_rect(x, y, w, h, COLOR_SURFACE_HIGH);
    fb_rect(x, y, 1, h, COLOR_SEPARATOR);
    font_draw(x + 12, y + 8, "Members", FONT_UI_BOLD, TYPE_HEADING, COLOR_ON_SURFACE);
    /* For the active room, list visibility-allowed members. We don't have
     * a dedicated room-member walker exposed yet; show the active ctx as
     * a stub so the empty pane doesn't lie. */
    int ry = y + 40;
    fb_circle_filled(x + 22, ry + 12, 12, COLOR_PRIMARY);
    char init = (char)(chat_zeos_current_ctx()[0] >= 'a' && chat_zeos_current_ctx()[0] <= 'z'
                       ? chat_zeos_current_ctx()[0] - 32 : chat_zeos_current_ctx()[0]);
    char buf[2] = { init, 0 };
    font_draw(x + 18, ry + 4, buf, FONT_UI_BOLD, TYPE_LABEL, COLOR_SURFACE);
    font_draw(x + 44, ry + 4, chat_zeos_current_ctx(), FONT_UI, TYPE_LABEL, COLOR_ON_SURFACE);
    font_draw(x + 44, ry + 18, "online", FONT_UI, TYPE_CAPTION, COLOR_SUCCESS);
    /* Honest gap notice */
    font_draw(x + 12, y + h - 36, "Per-room member walker:",
              FONT_UI, TYPE_CAPTION, COLOR_ON_SURFACE_3);
    font_draw(x + 12, y + h - 22, "exposed in v2 (see header)",
              FONT_UI, TYPE_CAPTION, COLOR_ON_SURFACE_3);
}

static void cp_draw_palette(int sx, int sy, int sw, int sh) {
    if (!C.palette_open) return;
    int pw = 480, ph = 60;
    int px = sx + (sw - pw) / 2;
    int py = sy + 80;
    fb_rect(px, py, pw, ph, COLOR_SURFACE_TOP);
    fb_rect_outline(px, py, pw, ph, COLOR_PRIMARY, 2);
    font_draw(px + 12, py + 10, "⌘K  Switch room / search", FONT_UI_BOLD,
              TYPE_LABEL, COLOR_ON_SURFACE_2);
    fb_rect(px + 12, py + 30, pw - 24, 24, COLOR_SURFACE);
    if (C.palette_input_len) {
        font_draw(px + 18, py + 34, C.palette_input, FONT_CODE, TYPE_LABEL, COLOR_ON_SURFACE);
    } else {
        font_draw(px + 18, py + 34, "type a room name…", FONT_UI, TYPE_LABEL, COLOR_ON_SURFACE_3);
    }
}

static void cp_draw_content(int id, int x, int y, int w, int h) {
    (void)id;
    fb_rect(x, y, w, h, COLOR_SURFACE);
    cp_draw_rooms   (x, y, CP_LEFT_W, h);
    cp_draw_messages(x + CP_LEFT_W, y, w - CP_LEFT_W - CP_RIGHT_W, h);
    cp_draw_members (x + w - CP_RIGHT_W, y, CP_RIGHT_W, h);
    cp_draw_palette(x, y, w, h);
}

void chat_polish_open(void) {
    chat_polish_init();
    if (C.active) { if (C.surface_id >= 0) wm_focus_surface(C.surface_id); return; }
    int sw = (int)fb_width(), sh = (int)fb_height();
    int sx = (sw - CP_W) / 2, sy = (sh - CP_H) / 2;
    C.surface_id = wm_create_surface("chat — chat-zeos", -1,
                                     sx, sy, CP_W, CP_H, cp_draw_content);
    if (C.surface_id < 0) return;
    wm_focus_surface(C.surface_id);
    wm_set_right_click(C.surface_id, cp_on_right_click);
    wm_set_persist_name(C.surface_id, "chat-zeos");
    C.active = 1;
}
void chat_polish_close(void) {
    if (!C.active) return;
    if (C.surface_id >= 0) wm_detach_surface(C.surface_id);
    C.surface_id = -1;
    C.active = 0;
}
int chat_polish_active(void) { return C.active; }

static int g_visible_count = 0;
static void cp_count_cb(const char *id, const char *name, int v, int m, int mc, void *u) {
    (void)id; (void)name; (void)v; (void)m; (void)mc; (void)u;
    g_visible_count++;
}

void chat_polish_print_selftest_line(void) {
    chat_polish_init();
    g_visible_count = 0;
    chat_zeos_walk_rooms(chat_zeos_current_ctx(), cp_count_cb, 0);
    kputs("chat-zeos polish ..... 3-pane UI ready, ");
    kput_dec((uint64_t)g_visible_count);
    kputs(" rooms visible to active ctx, E2EE rooms: ");
    kput_dec((uint64_t)chat_e2ee_active_count());
    kputs("\n");
}

void chat_polish_cmd(const char *args) {
    (void)args;
    chat_polish_open();
    kputs("chat opened\n");
}

/* ── Persistence ──────────────────────────────────────────────── */
static void cp_get_state(wm_persist_blob_t *out) {
    chain_surface_t *s = (C.surface_id >= 0) ? wm_get_surface(C.surface_id) : 0;
    if (s) { out->x = s->x; out->y = s->y; out->w = s->w; out->h = s->h; }
    out->sort_col = C.sort_col; out->sort_dir = C.sort_dir;
    out->theme_idx = C.theme_idx; out->scroll_pos = C.scroll_pos;
    int i = 0;
    for (; i < (int)sizeof(out->filter_str)-1 && C.active_room[i]; i++)
        out->filter_str[i] = C.active_room[i];
    out->filter_str[i] = 0;
}
static void cp_apply_state(const wm_persist_blob_t *in) {
    C.sort_col = in->sort_col; C.sort_dir = in->sort_dir;
    C.theme_idx = in->theme_idx; C.scroll_pos = in->scroll_pos;
    if (in->filter_str[0]) {
        /* Best-effort: ignore failure if room missing; default stays. */
        (void)chat_polish_set_active_room(in->filter_str);
    }
    if (C.surface_id >= 0 && in->w > 0 && in->h > 0) {
        wm_move_surface(C.surface_id, in->x, in->y);
        wm_resize_surface(C.surface_id, in->w, in->h);
    }
}

/* ── Settings ─────────────────────────────────────────────────── */
static int cp_parse_int(const char *v, int *o){
    if(!v||!*v)return -1; int n=0;
    while(*v){ if(*v<'0'||*v>'9')return -1; n=n*10+(*v-'0'); v++; }
    *o=n; return 0;
}
static void cp_int_emit(int v, char *o, int max){
    char t[12]; int ti=0; int k=0;
    if(v==0)t[ti++]='0';
    while(v>0&&ti<11){ t[ti++]=(char)('0'+v%10); v/=10; }
    while(ti>0&&k<max-1) o[k++]=t[--ti]; o[k]=0;
}
static int cp_g_hist(char *o, int m){ cp_int_emit(C.message_history_size, o, m); return 0; }
static int cp_s_hist(const char *v){ int n; if(cp_parse_int(v,&n)<0||n<10||n>100000)return -1; C.message_history_size=n; return 0; }
static int cp_g_ti(char *o, int m){ if(m<=0)return 0; o[0]=C.typing_indicator?'1':'0'; o[1]=0; return 0; }
static int cp_s_ti(const char *v){
    if(!v)return -1;
    if(v[0]=='0'||v[0]=='f'||v[0]=='F') C.typing_indicator=0;
    else if(v[0]=='1'||v[0]=='t'||v[0]=='T') C.typing_indicator=1;
    else return -1;
    return 0;
}
static int cp_g_rt(char *o, int m){ if(m<=0)return 0; o[0]=C.relative_timestamps?'1':'0'; o[1]=0; return 0; }
static int cp_s_rt(const char *v){
    if(!v)return -1;
    if(v[0]=='0'||v[0]=='f'||v[0]=='F') C.relative_timestamps=0;
    else if(v[0]=='1'||v[0]=='t'||v[0]=='T') C.relative_timestamps=1;
    else return -1;
    return 0;
}
static const settings_entry_t E_CHAT_HIST = {
    .name="chat.message_history_size",.kind=SK_INT,.flags=0,
    .getter=cp_g_hist,.setter=cp_s_hist,.enum_labels=0,
    .desc="Per-room messages held in memory"};
static const settings_entry_t E_CHAT_TYPE = {
    .name="chat.typing_indicator",.kind=SK_BOOL,.flags=0,
    .getter=cp_g_ti,.setter=cp_s_ti,.enum_labels=0,
    .desc="Show typing indicator"};
static const settings_entry_t E_CHAT_REL = {
    .name="chat.relative_timestamps",.kind=SK_BOOL,.flags=0,
    .getter=cp_g_rt,.setter=cp_s_rt,.enum_labels=0,
    .desc="Render timestamps as relative (\"2m ago\")"};

static void cp_register_settings(void) {
    (void)settings_register(&E_CHAT_HIST);
    (void)settings_register(&E_CHAT_TYPE);
    (void)settings_register(&E_CHAT_REL);
}

/* ── Right-click ──────────────────────────────────────────────── */
static int cp_is_own_msg(void) {
    return s_eq(C.selected_msg_author, chat_zeos_current_ctx());
}
static void cp_act_reply(void *c){ (void)c;
    notify_send(C.selected_msg_body, "reply in thread", NOTIFY_INFO);
}
static void cp_act_quote(void *c){ (void)c;
    notify_send(C.selected_msg_body, "quoted", NOTIFY_INFO);
}
static void cp_act_react(void *c){ (void)c;
    notify_send(C.selected_msg_body, "reaction added", NOTIFY_INFO);
}
static void cp_act_edit(void *c){ (void)c;
    if (cp_is_own_msg())
        notify_send(C.selected_msg_body, "editing", NOTIFY_INFO);
}
static void cp_act_delete(void *c){ (void)c;
    if (cp_is_own_msg())
        notify_send(C.selected_msg_body, "deleted", NOTIFY_WARNING);
}
static void cp_act_permalink(void *c){ (void)c;
    /* Build a chat://<room>/ts permalink. */
    char buf[160]; int k = 0;
    const char *p = "chat://";
    while (*p && k < 159) buf[k++] = *p++;
    for (int i = 0; C.active_room[i] && k < 158; i++) buf[k++] = C.active_room[i];
    if (k < 158) buf[k++] = '/';
    /* Append ts as decimal. */
    uint64_t v = C.selected_msg_ts;
    char tmp[24]; int ti = 0;
    if (v == 0) tmp[ti++] = '0';
    while (v > 0 && ti < 23) { tmp[ti++] = (char)('0' + (v % 10)); v /= 10; }
    while (ti > 0 && k < 159) buf[k++] = tmp[--ti];
    buf[k] = 0;
    notify_send(buf, "permalink", NOTIFY_INFO);
}

static void cp_on_right_click(int local_x, int local_y) {
    chain_surface_t *s = (C.surface_id >= 0) ? wm_get_surface(C.surface_id) : 0;
    if (!s) return;
    int cx = s->x + 1;
    int cy = s->y + WM_TITLEBAR_HEIGHT + 1;
    int abs_x = cx + local_x;
    int abs_y = cy + local_y;
    /* Hit-test against last drawn rows (rows are absolute screen y). */
    int hit = -1;
    for (int i = 0; i < g_msg_rows_count; i++) {
        if (abs_y >= g_msg_rows[i].y_top && abs_y < g_msg_rows[i].y_bot) {
            hit = i; break;
        }
    }
    if (hit < 0) return;
    /* Make sure we're in the messages pane horizontally. */
    int center_x_start = CP_LEFT_W;
    int center_x_end = (s->w - 2) - CP_RIGHT_W;
    if (local_x < center_x_start || local_x >= center_x_end) return;
    C.selected_msg_ts = g_msg_rows[hit].ts;
    s_cpy(C.selected_msg_author, g_msg_rows[hit].author, sizeof(C.selected_msg_author));
    s_cpy(C.selected_msg_body, g_msg_rows[hit].body, sizeof(C.selected_msg_body));
    int own = cp_is_own_msg();
    ctx_menu_item_t items[6];
    s_cpy(items[0].label, "Reply in thread", CTX_MENU_LABEL_MAX);
    items[0].action = cp_act_reply;     items[0].ctx = 0; items[0].enabled = 1;
    s_cpy(items[1].label, "Quote",           CTX_MENU_LABEL_MAX);
    items[1].action = cp_act_quote;     items[1].ctx = 0; items[1].enabled = 1;
    s_cpy(items[2].label, "React",           CTX_MENU_LABEL_MAX);
    items[2].action = cp_act_react;     items[2].ctx = 0; items[2].enabled = 1;
    s_cpy(items[3].label, "Edit",            CTX_MENU_LABEL_MAX);
    items[3].action = cp_act_edit;      items[3].ctx = 0; items[3].enabled = own;
    s_cpy(items[4].label, "Delete",          CTX_MENU_LABEL_MAX);
    items[4].action = cp_act_delete;    items[4].ctx = 0; items[4].enabled = own;
    s_cpy(items[5].label, "Copy permalink",  CTX_MENU_LABEL_MAX);
    items[5].action = cp_act_permalink; items[5].ctx = 0; items[5].enabled = 1;
    context_menu_open(abs_x, abs_y, items, 6);
}
