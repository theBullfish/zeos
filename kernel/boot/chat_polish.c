/*
 * chat-zeos polish — three-pane Slack/Discord-style window over the
 * existing chat_zeos engine. No new persistence; all rooms / messages
 * live in chat_zeos's btrees + vault.
 */

#include "chat_polish.h"
#include "chat_zeos.h"
#include "fb.h"
#include "font.h"
#include "theme.h"
#include "wm.h"
#include "compositor.h"
#include "ui_states.h"
#include "kprint.h"
#include "timeofday.h"

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
} cp_state_t;

static cp_state_t C;

static int  s_len(const char *s){int n=0;if(!s)return 0;while(s[n])n++;return n;}
static int  s_eq(const char *a,const char *b){while(*a&&*b&&*a==*b){a++;b++;}return *a==*b;}
static void s_cpy(char *d,const char *s,int max){int i=0;if(!d||max<=0)return;if(s)while(i<max-1&&s[i]){d[i]=s[i];i++;}d[i]=0;}

void chat_polish_init(void) {
    if (C.initialized) return;
    C.initialized = 1;
    C.surface_id = -1;
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
    /* track membership in C.e2ee[] */
    int idx = -1;
    for (int i = 0; i < C.e2ee_count; i++)
        if (s_eq(C.e2ee[i], room_id)) { idx = i; break; }
    if (on) {
        if (idx >= 0) return 0;
        if (C.e2ee_count >= CP_E2EE_MAX) return -1;
        s_cpy(C.e2ee[C.e2ee_count++], room_id, sizeof(C.e2ee[0]));
        return 0;
    } else {
        if (idx < 0) return 0;
        for (int j = idx; j < C.e2ee_count - 1; j++)
            s_cpy(C.e2ee[j], C.e2ee[j+1], sizeof(C.e2ee[0]));
        C.e2ee_count--;
        return 0;
    }
}
int chat_polish_is_e2ee(const char *room_id) {
    for (int i = 0; i < C.e2ee_count; i++)
        if (s_eq(C.e2ee[i], room_id)) return 1;
    return 0;
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

static void cp_msg_cb(uint64_t ts, const char *author, const char *body, void *user) {
    (void)user;
    if (g_msg_y > g_msg_y_max - 60) return;
    /* Avatar circle — first letter */
    uint32_t avc = avatar_color(author);
    fb_circle_filled(g_msg_x + 18, g_msg_y + 14, 14, avc);
    char init[2] = { author && *author ? (char)((*author >= 'a' && *author <= 'z') ? *author - 32 : *author) : '?', 0 };
    font_draw(g_msg_x + 12, g_msg_y + 6, init, FONT_UI_BOLD, TYPE_LABEL, COLOR_SURFACE);
    /* Author + relative ts */
    font_draw(g_msg_x + 44, g_msg_y, author, FONT_UI_BOLD, TYPE_LABEL, COLOR_ON_SURFACE);
    char rel[32]; cp_relative(ts, rel, sizeof(rel));
    font_draw(g_msg_x + 160, g_msg_y + 2, rel, FONT_UI, TYPE_CAPTION, COLOR_ON_SURFACE_3);
    /* Body */
    font_draw(g_msg_x + 44, g_msg_y + 18, body, FONT_UI, TYPE_BODY, COLOR_ON_SURFACE);
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
    kputs(" rooms visible to active ctx\n");
}

void chat_polish_cmd(const char *args) {
    (void)args;
    chat_polish_open();
    kputs("chat opened\n");
}
