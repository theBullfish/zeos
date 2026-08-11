/*
 * File manager: stream renderer over CHAIN_FS_EVENT. Every FS
 * mutation in the system flows through the chain, regardless of
 * who initiated it. UI subscribes; trash-GC subscribes;
 * search-index subscribes; undo subscribes. The "manager" is just
 * one of many consumers.
 *
 * The UI does NOT poll. It registers an fs_event listener and
 * marks the listing dirty when an event lands inside the currently
 * displayed directory. The directory snapshot is read from FAT32
 * once per refresh; the listener is what drives refresh, not a
 * timer. ui_states.h gives us empty/loading/error rendering.
 *
 * VAULT: recent locations + last window position persist.
 */

#include "file_mgr.h"
#include "fs_event.h"
#include "fat32.h"
#include "fb.h"
#include "font.h"
#include "theme.h"
#include "compositor.h"
#include "ui_dirty.h"
#include "ui_states.h"
#include "vault.h"
#include "kprint.h"
#include "timer.h"
#include "timeofday.h"

/* ── Layout ─────────────────────────────────────────────────────── */
#define FM_W            860
#define FM_H            560
#define FM_TITLE_H      32
#define FM_TOOLBAR_H    32
#define FM_BREAD_H      24
#define FM_STATUS_H     22
#define FM_PLACES_W     160
#define FM_PREVIEW_W    220
#define FM_ROW_H        20
#define FM_FONT_PX      13
#define FM_PAD          6

/* ── Tiny helpers ────────────────────────────────────────────────── */
static int fm_strlen(const char *s){int n=0;if(!s)return 0;while(s[n])n++;return n;}
static int fm_streq(const char *a, const char *b){
    if(!a||!b) return 0;
    while(*a && *b){ if(*a!=*b) return 0; a++; b++; }
    return *a==*b;
}
static void fm_strncpy(char *d, const char *s, int max){
    int i=0; if(!d||max<=0)return;
    if(s) while(i<max-1 && s[i]){d[i]=s[i];i++;}
    d[i]=0;
}
static void fm_strcat(char *d, const char *s, int max){
    int n = fm_strlen(d);
    if (n >= max-1) return;
    int i = 0;
    if (s) while (n < max-1 && s[i]) d[n++] = s[i++];
    d[n] = 0;
}
static void fm_itoa(uint32_t v, char *out){
    char buf[16]; int n=0;
    if(v==0){out[0]='0';out[1]=0;return;}
    while(v && n<15){buf[n++]=(char)('0'+(v%10));v/=10;}
    int k=0; while(n>0)out[k++]=buf[--n]; out[k]=0;
}
/* (range-select / toggle / starts_with retained for future shift- and
 * ctrl-modifier dispatch from the keyboard layer; suppressed-warning
 * tags kept the build clean while the modifiers get plumbed through.) */
__attribute__((unused))
static int starts_with(const char *s, const char *p){
    while(*p){ if(*s!=*p) return 0; s++; p++; }
    return 1;
}

/* ── Entry table ────────────────────────────────────────────────── */
typedef struct {
    char     name[64];
    uint32_t size;
    uint8_t  is_dir;
    uint8_t  selected;
} fm_entry_t;

/* Trash entry (for /trash subview) */
typedef struct {
    char     id[20];
    char     orig_path[FM_PATH_MAX];
    uint64_t deleted_at;
    uint32_t size;
} fm_trash_entry_t;

/* ── State ──────────────────────────────────────────────────────── */
typedef struct {
    int     active;
    int     wx, wy;
    char    cwd[FM_PATH_MAX];

    /* listing */
    fm_entry_t  entries[FM_ENTRIES_MAX];
    int         entry_count;
    int         scroll;
    int         focus_idx;            /* keyboard focus row */
    int         last_click_idx;       /* for shift-range select */
    list_state_t list_state;
    char        error_msg[64];
    int         dirty_listing;        /* listener marks this; tick refreshes */

    /* trash subview */
    int         in_trash;
    fm_trash_entry_t trash[FM_ENTRIES_MAX];
    int         trash_count;

    /* navigation history (back/forward) */
    char        hist[FM_HISTORY_MAX][FM_PATH_MAX];
    int         hist_count;
    int         hist_pos;             /* current index */

    /* recents (persisted) */
    char        recents[FM_RECENT_MAX][FM_PATH_MAX];
    int         recent_count;

    /* preview */
    int         preview_idx;          /* which entry is selected for preview */

    /* drag state */
    int         drag_active;
    int         drag_idx;
} fm_state_t;

static fm_state_t g_fm;

/* ── Counters ───────────────────────────────────────────────────── */
static uint32_t g_total_listings;
static uint32_t g_total_events_seen;

uint32_t file_mgr_total_listings(void)    { return g_total_listings; }
uint32_t file_mgr_total_events_seen(void) { return g_total_events_seen; }

/* ── Forward decls ──────────────────────────────────────────────── */
static void fm_refresh_listing(void);
static void fm_refresh_trash(void);
static void fm_persist_save_internal(void);
static void fm_navigate_to(const char *path, int push_history);
static void fm_listener(const fs_event_t *e, void *ctx);

/* ── Path helpers ───────────────────────────────────────────────── */
static void fm_parent_of(const char *path, char *out, int max){
    int n = fm_strlen(path);
    int last = -1;
    for (int i = 0; i < n; i++) if (path[i] == '/') last = i;
    if (last <= 0) { fm_strncpy(out, "/", max); return; }
    int copy = last;
    if (copy >= max) copy = max - 1;
    for (int i = 0; i < copy; i++) out[i] = path[i];
    out[copy] = 0;
}

static void fm_join(const char *dir, const char *name, char *out, int max){
    fm_strncpy(out, dir, max);
    int n = fm_strlen(out);
    if (n == 0 || out[n-1] != '/') {
        if (n < max-1) { out[n++] = '/'; out[n] = 0; }
    }
    fm_strcat(out, name, max);
}

/* ── Recent locations ───────────────────────────────────────────── */
static void fm_record_recent(const char *path) {
    if (!path || !path[0]) return;
    /* If already present, move to front. */
    int found = -1;
    for (int i = 0; i < g_fm.recent_count; i++)
        if (fm_streq(g_fm.recents[i], path)) { found = i; break; }
    if (found < 0) {
        if (g_fm.recent_count < FM_RECENT_MAX) {
            for (int i = g_fm.recent_count; i > 0; i--)
                fm_strncpy(g_fm.recents[i], g_fm.recents[i-1], FM_PATH_MAX);
            fm_strncpy(g_fm.recents[0], path, FM_PATH_MAX);
            g_fm.recent_count++;
        } else {
            for (int i = FM_RECENT_MAX-1; i > 0; i--)
                fm_strncpy(g_fm.recents[i], g_fm.recents[i-1], FM_PATH_MAX);
            fm_strncpy(g_fm.recents[0], path, FM_PATH_MAX);
        }
    } else if (found > 0) {
        char tmp[FM_PATH_MAX];
        fm_strncpy(tmp, g_fm.recents[found], FM_PATH_MAX);
        for (int i = found; i > 0; i--)
            fm_strncpy(g_fm.recents[i], g_fm.recents[i-1], FM_PATH_MAX);
        fm_strncpy(g_fm.recents[0], tmp, FM_PATH_MAX);
    }
}

/* ── Listing refresh ────────────────────────────────────────────── */

struct fm_readdir_acc {
    fm_entry_t *e;
    int        *count;
};

static int fm_readdir_cb(const char *name, uint32_t size,
                         uint32_t cluster, uint8_t attr, void *user) {
    (void)cluster;
    struct fm_readdir_acc *a = (struct fm_readdir_acc *)user;
    if (*a->count >= FM_ENTRIES_MAX) return 1;
    fm_entry_t *e = &a->e[*a->count];
    fm_strncpy(e->name, name, sizeof(e->name));
    e->size = size;
    e->is_dir = (attr & 0x10) ? 1 : 0;     /* FAT32_ATTR_DIR */
    e->selected = 0;
    (*a->count)++;
    return 0;
}

static void fm_refresh_listing(void) {
    if (g_fm.in_trash) { fm_refresh_trash(); return; }
    g_fm.list_state = LIST_LOADING;
    g_fm.entry_count = 0;
    g_fm.error_msg[0] = 0;

    if (!fat32_mounted()) {
        g_fm.list_state = LIST_ERROR;
        fm_strncpy(g_fm.error_msg, "no FAT32 mount", sizeof(g_fm.error_msg));
        return;
    }

    struct fm_readdir_acc acc;
    acc.e = g_fm.entries;
    acc.count = &g_fm.entry_count;
    int rc = fat32_readdir(g_fm.cwd, fm_readdir_cb, &acc);
    if (rc < 0) {
        g_fm.list_state = LIST_ERROR;
        fm_strncpy(g_fm.error_msg, "directory not readable",
                   sizeof(g_fm.error_msg));
        return;
    }
    g_fm.list_state = (g_fm.entry_count == 0) ? LIST_EMPTY : LIST_OK;
    g_fm.dirty_listing = 0;
    g_total_listings++;
    if (g_fm.preview_idx >= g_fm.entry_count) g_fm.preview_idx = -1;
    if (g_fm.focus_idx >= g_fm.entry_count) g_fm.focus_idx = g_fm.entry_count - 1;
    if (g_fm.focus_idx < 0 && g_fm.entry_count > 0) g_fm.focus_idx = 0;
}

/* ── Trash subview ──────────────────────────────────────────────── */

static int s_trash_collect_count;
static void fm_trash_collect_cb(const char *id, const char *orig_path,
                                uint64_t deleted_at, uint32_t size) {
    if (s_trash_collect_count >= FM_ENTRIES_MAX) return;
    fm_trash_entry_t *t = &g_fm.trash[s_trash_collect_count++];
    fm_strncpy(t->id, id, sizeof(t->id));
    fm_strncpy(t->orig_path, orig_path, sizeof(t->orig_path));
    t->deleted_at = deleted_at;
    t->size       = size;
}

static void fm_refresh_trash(void) {
    g_fm.list_state = LIST_LOADING;
    g_fm.trash_count = 0;
    s_trash_collect_count = 0;
    if (!fat32_mounted()) {
        g_fm.list_state = LIST_ERROR;
        fm_strncpy(g_fm.error_msg, "no FAT32 mount", sizeof(g_fm.error_msg));
        return;
    }
    int rc = fat32_trash_list(fm_trash_collect_cb);
    g_fm.trash_count = (rc < 0) ? 0 : s_trash_collect_count;
    if (rc < 0) {
        g_fm.list_state = LIST_ERROR;
        fm_strncpy(g_fm.error_msg, "trash unreadable", sizeof(g_fm.error_msg));
        return;
    }
    g_fm.list_state = (g_fm.trash_count == 0) ? LIST_EMPTY : LIST_OK;
    g_fm.dirty_listing = 0;
    g_total_listings++;
}

/* ── Listener (fired by every fs_event in the system) ───────────── */
static void fm_listener(const fs_event_t *e, void *ctx) {
    (void)ctx;
    if (!g_fm.active || !e || !e->valid) return;
    g_total_events_seen++;
    /* If the event touches our cwd directly, mark dirty. We compare
     * the parent dir of e->path against cwd. */
    char dir[FM_PATH_MAX];
    fm_parent_of(e->path, dir, FM_PATH_MAX);
    if (g_fm.in_trash) {
        if (e->kind == FS_EV_TRASH || e->kind == FS_EV_RESTORE) {
            g_fm.dirty_listing = 1;
            dirty_register(FM_WIN_ID);
            compositor_dirty(g_fm.wx, g_fm.wy, FM_W, FM_H);
        }
        return;
    }
    if (fm_streq(dir, g_fm.cwd) || fm_streq(e->path, g_fm.cwd)) {
        g_fm.dirty_listing = 1;
        dirty_register(FM_WIN_ID);
        compositor_dirty(g_fm.wx, g_fm.wy, FM_W, FM_H);
    }
}

/* ── Navigation ─────────────────────────────────────────────────── */
static void fm_navigate_to(const char *path, int push_history) {
    if (!path || !path[0]) return;

    int trash_view = fm_streq(path, "/trash") || fm_streq(path, "/.zeos-trash");
    g_fm.in_trash = trash_view ? 1 : 0;

    if (!trash_view) fm_strncpy(g_fm.cwd, path, FM_PATH_MAX);
    else             fm_strncpy(g_fm.cwd, "/trash", FM_PATH_MAX);

    if (push_history) {
        /* Drop forward branch */
        if (g_fm.hist_pos < g_fm.hist_count - 1)
            g_fm.hist_count = g_fm.hist_pos + 1;
        if (g_fm.hist_count >= FM_HISTORY_MAX) {
            for (int i = 0; i < FM_HISTORY_MAX - 1; i++)
                fm_strncpy(g_fm.hist[i], g_fm.hist[i+1], FM_PATH_MAX);
            g_fm.hist_count = FM_HISTORY_MAX - 1;
        }
        fm_strncpy(g_fm.hist[g_fm.hist_count], g_fm.cwd, FM_PATH_MAX);
        g_fm.hist_count++;
        g_fm.hist_pos = g_fm.hist_count - 1;
    }

    g_fm.scroll = 0;
    g_fm.focus_idx = -1;
    g_fm.preview_idx = -1;
    g_fm.last_click_idx = -1;
    fm_record_recent(g_fm.cwd);
    fm_refresh_listing();
    fm_persist_save_internal();
    compositor_dirty(g_fm.wx, g_fm.wy, FM_W, FM_H);
}

/* ── Public open/close ──────────────────────────────────────────── */

static void fm_layout_origin(void) {
    int sw = (int)fb_width(), sh = (int)fb_height();
    g_fm.wx = (sw - FM_W) / 2;
    g_fm.wy = (sh - FM_H) / 2;
    if (g_fm.wx < 0) g_fm.wx = 0;
    if (g_fm.wy < 0) g_fm.wy = 0;
}

static int s_listener_registered;

int file_mgr_open(const char *path) {
    if (!g_fm.active) {
        /* zero everything except recents — those persist across opens */
        char saved_recents[FM_RECENT_MAX][FM_PATH_MAX];
        int  saved_recent_count = g_fm.recent_count;
        for (int i = 0; i < FM_RECENT_MAX; i++)
            fm_strncpy(saved_recents[i], g_fm.recents[i], FM_PATH_MAX);

        for (uint32_t i = 0; i < sizeof(g_fm); i++)
            ((uint8_t *)&g_fm)[i] = 0;
        g_fm.active = 1;
        g_fm.focus_idx = -1;
        g_fm.preview_idx = -1;
        g_fm.last_click_idx = -1;
        g_fm.recent_count = saved_recent_count;
        for (int i = 0; i < FM_RECENT_MAX; i++)
            fm_strncpy(g_fm.recents[i], saved_recents[i], FM_PATH_MAX);
    }
    fm_layout_origin();
    if (!s_listener_registered) {
        fs_event_register_listener(fm_listener, 0);
        s_listener_registered = 1;
    }
    const char *p = (path && path[0]) ? path : "/";
    fm_navigate_to(p, 1);
    return 0;
}

void file_mgr_close(void) {
    if (!g_fm.active) return;
    g_fm.active = 0;
    fm_persist_save_internal();
    int sw = (int)fb_width(), sh = (int)fb_height();
    compositor_dirty(0, 0, sw, sh);
}

int file_mgr_active(void) { return g_fm.active; }

/* ── Selection helpers ──────────────────────────────────────────── */
static void fm_select_only(int idx) {
    for (int i = 0; i < g_fm.entry_count; i++) g_fm.entries[i].selected = 0;
    if (idx >= 0 && idx < g_fm.entry_count) g_fm.entries[idx].selected = 1;
    g_fm.preview_idx = idx;
}
__attribute__((unused))
static void fm_toggle(int idx) {
    if (idx < 0 || idx >= g_fm.entry_count) return;
    g_fm.entries[idx].selected = !g_fm.entries[idx].selected;
    if (g_fm.entries[idx].selected) g_fm.preview_idx = idx;
}
__attribute__((unused))
static void fm_select_range(int from, int to) {
    if (from < 0) { fm_select_only(to); return; }
    int a = from < to ? from : to;
    int b = from < to ? to : from;
    for (int i = 0; i < g_fm.entry_count; i++) g_fm.entries[i].selected = 0;
    for (int i = a; i <= b && i < g_fm.entry_count; i++) g_fm.entries[i].selected = 1;
    g_fm.preview_idx = to;
}

/* ── Operations ─────────────────────────────────────────────────── */
static void fm_op_delete_selected(void) {
    if (g_fm.in_trash) return;
    for (int i = 0; i < g_fm.entry_count; i++) {
        if (!g_fm.entries[i].selected) continue;
        if (g_fm.entries[i].is_dir) continue;  /* dirs need recursive trash */
        char p[FM_PATH_MAX];
        fm_join(g_fm.cwd, g_fm.entries[i].name, p, FM_PATH_MAX);
        (void)fat32_trash(p, "user", 0);
    }
    /* listener will mark dirty; refresh now too for immediate feedback */
    fm_refresh_listing();
}

static void fm_op_new_folder(void) {
    if (g_fm.in_trash) return;
    /* generate "New Folder", "New Folder 2", etc. */
    for (int n = 1; n < 99; n++) {
        char name[32];
        if (n == 1) fm_strncpy(name, "NewFolder", sizeof(name));
        else {
            fm_strncpy(name, "NewFolder", sizeof(name));
            char ns[8]; fm_itoa((uint32_t)n, ns);
            fm_strcat(name, ns, sizeof(name));
        }
        char p[FM_PATH_MAX];
        fm_join(g_fm.cwd, name, p, FM_PATH_MAX);
        if (fat32_mkdir(p) == 0) break;
    }
    fm_refresh_listing();
}

static void fm_op_new_file(void) {
    if (g_fm.in_trash) return;
    for (int n = 1; n < 99; n++) {
        char name[32];
        if (n == 1) fm_strncpy(name, "untitled.txt", sizeof(name));
        else {
            fm_strncpy(name, "untitled", sizeof(name));
            char ns[8]; fm_itoa((uint32_t)n, ns);
            fm_strcat(name, ns, sizeof(name));
            fm_strcat(name, ".txt", sizeof(name));
        }
        char p[FM_PATH_MAX];
        fm_join(g_fm.cwd, name, p, FM_PATH_MAX);
        if (fat32_create(p) == 0) break;
    }
    fm_refresh_listing();
}

static void fm_op_trash_restore(int idx) {
    if (idx < 0 || idx >= g_fm.trash_count) return;
    (void)fat32_trash_restore(g_fm.trash[idx].id);
    fm_refresh_trash();
}

static void fm_op_trash_purge_all(void) {
    (void)fat32_trash_empty();
    fm_refresh_trash();
}

/* ── Drawing ────────────────────────────────────────────────────── */

static void fm_draw_close_button(int wx, int wy) {
    int bx = wx + FM_W - 28, by = wy + 6;
    fb_rect(bx, by, 20, 20, COLOR_SURFACE_HIGH);
    font_draw(bx + 6, by + 3, "x", FONT_UI, FM_FONT_PX, COLOR_ON_SURFACE);
}

static void fm_draw_toolbar(int wx, int wy) {
    int ty = wy + FM_TITLE_H;
    fb_rect(wx, ty, FM_W, FM_TOOLBAR_H, COLOR_SURFACE_HIGH);
    /* Buttons: back, forward, up, new-folder, new-file, search */
    const char *labels[] = { "<", ">", "^", "+dir", "+file", "?" };
    int bx = wx + FM_PAD;
    for (int i = 0; i < 6; i++) {
        int bw = (i < 3) ? 24 : 56;
        fb_rect(bx, ty + 6, bw, 20, COLOR_SURFACE);
        fb_rect_outline(bx, ty + 6, bw, 20, COLOR_SEPARATOR, 1);
        font_draw(bx + 6, ty + 9, labels[i], FONT_UI, FM_FONT_PX, COLOR_ON_SURFACE);
        bx += bw + 6;
    }
}

static void fm_draw_breadcrumbs(int wx, int wy) {
    int by = wy + FM_TITLE_H + FM_TOOLBAR_H;
    fb_rect(wx, by, FM_W, FM_BREAD_H, COLOR_SURFACE);
    fb_rect_outline(wx, by, FM_W, FM_BREAD_H, COLOR_SEPARATOR, 1);
    font_draw(wx + FM_PAD, by + 5, g_fm.cwd, FONT_UI, FM_FONT_PX, COLOR_ON_SURFACE);
}

static void fm_draw_places(int wx, int wy) {
    int py = wy + FM_TITLE_H + FM_TOOLBAR_H + FM_BREAD_H;
    int ph = FM_H - FM_TITLE_H - FM_TOOLBAR_H - FM_BREAD_H - FM_STATUS_H;
    fb_rect(wx, py, FM_PLACES_W, ph, COLOR_SURFACE_HIGH);
    fb_rect_outline(wx, py, FM_PLACES_W, ph, COLOR_SEPARATOR, 1);

    const char *places[] = { "/", "/home", "/vault", "/trash" };
    font_draw(wx + FM_PAD, py + 4, "Places", FONT_UI, FM_FONT_PX, COLOR_ON_SURFACE_3);
    int yy = py + 24;
    for (int i = 0; i < 4; i++) {
        int active = fm_streq(g_fm.cwd, places[i]) ? 1 : 0;
        if (active) fb_rect(wx + 4, yy - 2, FM_PLACES_W - 8, FM_ROW_H, COLOR_PRIMARY);
        font_draw(wx + 12, yy + 2, places[i], FONT_UI, FM_FONT_PX,
                  active ? COLOR_ON_SURFACE : COLOR_ON_SURFACE);
        yy += FM_ROW_H;
    }

    /* Recents */
    yy += 8;
    font_draw(wx + FM_PAD, yy, "Recent", FONT_UI, FM_FONT_PX, COLOR_ON_SURFACE_3);
    yy += 18;
    for (int i = 0; i < g_fm.recent_count; i++) {
        font_draw(wx + 12, yy + 2, g_fm.recents[i], FONT_UI, FM_FONT_PX,
                  COLOR_ON_SURFACE_2);
        yy += FM_ROW_H;
    }
}

static void fm_draw_listing(int wx, int wy) {
    int lx = wx + FM_PLACES_W;
    int ly = wy + FM_TITLE_H + FM_TOOLBAR_H + FM_BREAD_H;
    int lw = FM_W - FM_PLACES_W - FM_PREVIEW_W;
    int lh = FM_H - FM_TITLE_H - FM_TOOLBAR_H - FM_BREAD_H - FM_STATUS_H;
    fb_rect(lx, ly, lw, lh, COLOR_SURFACE);
    fb_rect_outline(lx, ly, lw, lh, COLOR_SEPARATOR, 1);

    /* Empty / loading / error states */
    if (g_fm.list_state != LIST_OK) {
        list_state_ctx_t c = {
            .x = lx, .y = ly, .w = lw, .h = lh,
            .state = g_fm.list_state,
            .message = g_fm.error_msg[0] ? g_fm.error_msg : 0,
            .cta = (g_fm.list_state == LIST_EMPTY) ? "Drop files here" : 0,
            .retry_cb = 0,
            .retry_ctx = 0,
        };
        list_render_state(&c);
        return;
    }

    int row_y = ly + 4;
    if (g_fm.in_trash) {
        for (int i = g_fm.scroll; i < g_fm.trash_count && row_y + FM_ROW_H < ly + lh; i++) {
            int focused = (i == g_fm.focus_idx);
            if (focused) fb_rect(lx + 2, row_y - 1, lw - 4, FM_ROW_H, COLOR_PRIMARY);
            font_draw(lx + 8, row_y + 2, g_fm.trash[i].orig_path,
                      FONT_UI, FM_FONT_PX, focused ? COLOR_ON_SURFACE : COLOR_ON_SURFACE);
            char ns[16]; fm_itoa(g_fm.trash[i].size, ns);
            font_draw(lx + lw - 80, row_y + 2, ns, FONT_UI, FM_FONT_PX,
                      focused ? COLOR_ON_SURFACE : COLOR_ON_SURFACE_3);
            row_y += FM_ROW_H;
        }
        return;
    }

    for (int i = g_fm.scroll; i < g_fm.entry_count && row_y + FM_ROW_H < ly + lh; i++) {
        fm_entry_t *e = &g_fm.entries[i];
        int focused = (i == g_fm.focus_idx);
        if (e->selected) fb_rect(lx + 2, row_y - 1, lw - 4, FM_ROW_H, COLOR_PRIMARY);
        else if (focused) fb_rect_outline(lx + 2, row_y - 1, lw - 4, FM_ROW_H, COLOR_PRIMARY, 1);

        /* icon */
        const char *icon = e->is_dir ? "[d]" : "   ";
        font_draw(lx + 6, row_y + 2, icon, FONT_UI, FM_FONT_PX, COLOR_ON_SURFACE_3);
        font_draw(lx + 32, row_y + 2, e->name, FONT_UI, FM_FONT_PX,
                  e->selected ? COLOR_ON_SURFACE : COLOR_ON_SURFACE);
        if (!e->is_dir) {
            char ns[16]; fm_itoa(e->size, ns);
            font_draw(lx + lw - 80, row_y + 2, ns, FONT_UI, FM_FONT_PX,
                      e->selected ? COLOR_ON_SURFACE : COLOR_ON_SURFACE_3);
        }
        row_y += FM_ROW_H;
    }
}

static void fm_draw_preview(int wx, int wy) {
    int px = wx + FM_W - FM_PREVIEW_W;
    int py = wy + FM_TITLE_H + FM_TOOLBAR_H + FM_BREAD_H;
    int ph = FM_H - FM_TITLE_H - FM_TOOLBAR_H - FM_BREAD_H - FM_STATUS_H;
    fb_rect(px, py, FM_PREVIEW_W, ph, COLOR_SURFACE_HIGH);
    fb_rect_outline(px, py, FM_PREVIEW_W, ph, COLOR_SEPARATOR, 1);
    font_draw(px + FM_PAD, py + 4, "Preview", FONT_UI, FM_FONT_PX, COLOR_ON_SURFACE_3);

    if (g_fm.in_trash) {
        if (g_fm.focus_idx >= 0 && g_fm.focus_idx < g_fm.trash_count) {
            fm_trash_entry_t *t = &g_fm.trash[g_fm.focus_idx];
            font_draw(px + FM_PAD, py + 28, "id:", FONT_UI, FM_FONT_PX, COLOR_ON_SURFACE_3);
            font_draw(px + FM_PAD + 24, py + 28, t->id, FONT_UI, FM_FONT_PX, COLOR_ON_SURFACE);
            font_draw(px + FM_PAD, py + 48, "from:", FONT_UI, FM_FONT_PX, COLOR_ON_SURFACE_3);
            font_draw(px + FM_PAD, py + 64, t->orig_path, FONT_UI, FM_FONT_PX, COLOR_ON_SURFACE);
            char ns[16]; fm_itoa(t->size, ns);
            font_draw(px + FM_PAD, py + 88, "size:", FONT_UI, FM_FONT_PX, COLOR_ON_SURFACE_3);
            font_draw(px + FM_PAD + 40, py + 88, ns, FONT_UI, FM_FONT_PX, COLOR_ON_SURFACE);
            /* Restore button */
            int bx = px + FM_PAD;
            int by = py + ph - 60;
            fb_rect(bx, by, FM_PREVIEW_W - FM_PAD*2, 22, COLOR_PRIMARY);
            font_draw(bx + 8, by + 4, "Restore", FONT_UI, FM_FONT_PX, COLOR_ON_SURFACE);
            int by2 = py + ph - 32;
            fb_rect(bx, by2, FM_PREVIEW_W - FM_PAD*2, 22, COLOR_SURFACE);
            fb_rect_outline(bx, by2, FM_PREVIEW_W - FM_PAD*2, 22, COLOR_SEPARATOR, 1);
            font_draw(bx + 8, by2 + 4, "Empty trash", FONT_UI, FM_FONT_PX, COLOR_ON_SURFACE);
        }
        return;
    }

    if (g_fm.preview_idx < 0 || g_fm.preview_idx >= g_fm.entry_count) return;
    fm_entry_t *e = &g_fm.entries[g_fm.preview_idx];
    font_draw(px + FM_PAD, py + 28, e->name, FONT_UI, FM_FONT_PX, COLOR_ON_SURFACE);
    char buf[64];
    fm_strncpy(buf, e->is_dir ? "kind: directory" : "kind: file", sizeof(buf));
    font_draw(px + FM_PAD, py + 48, buf, FONT_UI, FM_FONT_PX, COLOR_ON_SURFACE_3);
    char ns[16]; fm_itoa(e->size, ns);
    fm_strncpy(buf, "size: ", sizeof(buf));
    fm_strcat(buf, ns, sizeof(buf));
    font_draw(px + FM_PAD, py + 64, buf, FONT_UI, FM_FONT_PX, COLOR_ON_SURFACE_3);

    /* Text preview for small files (<4KB): read first chunk via fat32. */
    if (!e->is_dir && e->size > 0 && e->size < 4096) {
        char path[FM_PATH_MAX];
        fm_join(g_fm.cwd, e->name, path, FM_PATH_MAX);
        struct fat32_file f;
        if (fat32_open(path, &f) == 0) {
            static char preview[1024];
            int n = fat32_read(&f, preview, sizeof(preview) - 1);
            if (n > 0) {
                preview[n] = 0;
                /* Replace non-printables with '.' */
                for (int i = 0; i < n; i++) {
                    if (preview[i] != '\n' && preview[i] != '\r' &&
                        preview[i] != '\t' &&
                        (preview[i] < 32 || preview[i] > 126))
                        preview[i] = '.';
                }
                /* Render line-by-line, up to ~10 lines. */
                int yy = py + 92;
                int line_start = 0;
                int lines = 0;
                for (int i = 0; i <= n && lines < 12; i++) {
                    if (i == n || preview[i] == '\n') {
                        char saved = preview[i];
                        preview[i] = 0;
                        font_draw(px + FM_PAD, yy, &preview[line_start],
                                  FONT_CODE, FM_FONT_PX, COLOR_ON_SURFACE_2);
                        preview[i] = saved;
                        yy += 14;
                        line_start = i + 1;
                        lines++;
                    }
                }
            }
        }
    }
}

void file_mgr_draw(void) {
    if (!g_fm.active) return;
    if (g_fm.dirty_listing) fm_refresh_listing();

    int wx = g_fm.wx, wy = g_fm.wy;
    fb_rect(wx, wy, FM_W, FM_H, COLOR_SURFACE);
    fb_rect_outline(wx, wy, FM_W, FM_H, COLOR_SURFACE_TOP, 1);

    /* Title */
    fb_rect(wx, wy, FM_W, FM_TITLE_H, COLOR_SURFACE_HIGH);
    font_draw(wx + FM_PAD, wy + 8, "Files", FONT_UI, FM_FONT_PX, COLOR_ON_SURFACE);
    fm_draw_close_button(wx, wy);

    fm_draw_toolbar(wx, wy);
    fm_draw_breadcrumbs(wx, wy);
    fm_draw_places(wx, wy);
    fm_draw_listing(wx, wy);
    fm_draw_preview(wx, wy);

    /* Status bar */
    int sy = wy + FM_H - FM_STATUS_H;
    fb_rect(wx, sy, FM_W, FM_STATUS_H, COLOR_SURFACE_HIGH);
    char status[128];
    int n = g_fm.in_trash ? g_fm.trash_count : g_fm.entry_count;
    fm_strncpy(status, g_fm.in_trash ? "trash: " : "items: ", sizeof(status));
    char ns[16]; fm_itoa((uint32_t)n, ns);
    fm_strcat(status, ns, sizeof(status));
    fm_strcat(status, "  |  events: ", sizeof(status));
    fm_itoa(g_total_events_seen, ns);
    fm_strcat(status, ns, sizeof(status));
    fm_strcat(status, "  |  undo: ", sizeof(status));
    fm_itoa((uint32_t)fs_event_undo_count(), ns);
    fm_strcat(status, ns, sizeof(status));
    font_draw(wx + FM_PAD, sy + 4, status, FONT_UI, FM_FONT_PX, COLOR_ON_SURFACE_2);
}

/* ── Input ──────────────────────────────────────────────────────── */

static int fm_in_window(int x, int y) {
    return (x >= g_fm.wx && y >= g_fm.wy &&
            x < g_fm.wx + FM_W && y < g_fm.wy + FM_H);
}

int file_mgr_key(int ascii) {
    if (!g_fm.active) return 0;
    if (ascii == 27) { file_mgr_close(); return 1; }
    if (ascii == '\n' || ascii == '\r') {
        /* enter on focused: open dir or file */
        if (g_fm.in_trash) {
            fm_op_trash_restore(g_fm.focus_idx);
            return 1;
        }
        if (g_fm.focus_idx < 0 || g_fm.focus_idx >= g_fm.entry_count) return 1;
        fm_entry_t *e = &g_fm.entries[g_fm.focus_idx];
        if (e->is_dir) {
            char p[FM_PATH_MAX];
            fm_join(g_fm.cwd, e->name, p, FM_PATH_MAX);
            fm_navigate_to(p, 1);
        } else {
            /* Open in editor for text-ish, otherwise just preview-only */
            extern int editor_open(const char *path);
            char p[FM_PATH_MAX];
            fm_join(g_fm.cwd, e->name, p, FM_PATH_MAX);
            (void)editor_open(p);
        }
        return 1;
    }
    if (ascii == 8 || ascii == 127) {
        /* Backspace = delete selected (to trash) */
        fm_op_delete_selected();
        return 1;
    }
    return 1;
}

int file_mgr_key_arrow(int dir) {
    if (!g_fm.active) return 0;
    int n = g_fm.in_trash ? g_fm.trash_count : g_fm.entry_count;
    if (n == 0) return 1;
    if (dir == 2) { /* up */
        if (g_fm.focus_idx > 0) g_fm.focus_idx--;
        else g_fm.focus_idx = 0;
    } else if (dir == 3) { /* down */
        if (g_fm.focus_idx < n - 1) g_fm.focus_idx++;
        else g_fm.focus_idx = n - 1;
    } else if (dir == 0) { /* left = back */
        if (g_fm.hist_pos > 0) {
            g_fm.hist_pos--;
            fm_navigate_to(g_fm.hist[g_fm.hist_pos], 0);
        }
    } else if (dir == 1) { /* right = forward */
        if (g_fm.hist_pos < g_fm.hist_count - 1) {
            g_fm.hist_pos++;
            fm_navigate_to(g_fm.hist[g_fm.hist_pos], 0);
        }
    }
    if (!g_fm.in_trash) fm_select_only(g_fm.focus_idx);
    return 1;
}

/* Toolbar hit-test helper */
static int fm_toolbar_button_at(int x, int y) {
    int wx = g_fm.wx, wy = g_fm.wy;
    int ty = wy + FM_TITLE_H;
    if (y < ty + 6 || y > ty + 26) return -1;
    int bx = wx + FM_PAD;
    int widths[] = { 24, 24, 24, 56, 56, 56 };
    for (int i = 0; i < 6; i++) {
        if (x >= bx && x < bx + widths[i]) return i;
        bx += widths[i] + 6;
    }
    return -1;
}

static int fm_places_hit(int x, int y) {
    int wx = g_fm.wx, wy = g_fm.wy;
    int py = wy + FM_TITLE_H + FM_TOOLBAR_H + FM_BREAD_H + 24;
    if (x < wx || x >= wx + FM_PLACES_W) return -1;
    int idx = (y - py) / FM_ROW_H;
    if (idx < 0 || idx >= 4) return -1;
    return idx;
}

static int fm_listing_hit(int x, int y) {
    int wx = g_fm.wx, wy = g_fm.wy;
    int lx = wx + FM_PLACES_W;
    int ly = wy + FM_TITLE_H + FM_TOOLBAR_H + FM_BREAD_H + 4;
    int lw = FM_W - FM_PLACES_W - FM_PREVIEW_W;
    if (x < lx || x >= lx + lw) return -1;
    int rel = (y - ly) / FM_ROW_H;
    if (rel < 0) return -1;
    int idx = rel + g_fm.scroll;
    int n = g_fm.in_trash ? g_fm.trash_count : g_fm.entry_count;
    if (idx >= n) return -1;
    return idx;
}

int file_mgr_mouse_down(int x, int y, int button) {
    if (!g_fm.active) return 0;
    if (!fm_in_window(x, y)) return 1;
    int wx = g_fm.wx, wy = g_fm.wy;
    /* Close button */
    if (x >= wx + FM_W - 28 && x <= wx + FM_W - 8 &&
        y >= wy + 6 && y <= wy + 26) {
        file_mgr_close();
        return 1;
    }
    /* Toolbar */
    int tb = fm_toolbar_button_at(x, y);
    if (tb >= 0) {
        switch (tb) {
            case 0: file_mgr_key_arrow(0); break;   /* back */
            case 1: file_mgr_key_arrow(1); break;   /* forward */
            case 2: {                                /* up */
                char par[FM_PATH_MAX];
                fm_parent_of(g_fm.cwd, par, FM_PATH_MAX);
                fm_navigate_to(par, 1);
                break;
            }
            case 3: fm_op_new_folder(); break;
            case 4: fm_op_new_file(); break;
            case 5: break; /* search stub */
        }
        return 1;
    }
    /* Places */
    int pl = fm_places_hit(x, y);
    if (pl >= 0) {
        const char *places[] = { "/", "/home", "/vault", "/trash" };
        fm_navigate_to(places[pl], 1);
        return 1;
    }
    /* Listing */
    int li = fm_listing_hit(x, y);
    if (li >= 0) {
        if (button == 2) {
            /* right-click: simple context menu - just trash for now */
            if (!g_fm.in_trash) {
                g_fm.focus_idx = li;
                fm_select_only(li);
                fm_op_delete_selected();
            }
            return 1;
        }
        g_fm.focus_idx = li;
        if (g_fm.in_trash) {
            return 1;
        }
        /* Drag start */
        g_fm.drag_active = 1;
        g_fm.drag_idx = li;
        /* For now: simple click = single-select. Full shift/ctrl handling
         * would require modifier state plumbing; click toggles a single
         * selection. */
        fm_select_only(li);
        g_fm.last_click_idx = li;
        return 1;
    }
    /* Preview pane buttons (trash subview restore/empty) */
    if (g_fm.in_trash) {
        int px = wx + FM_W - FM_PREVIEW_W + FM_PAD;
        int pw = FM_PREVIEW_W - FM_PAD*2;
        int ph = FM_H - FM_TITLE_H - FM_TOOLBAR_H - FM_BREAD_H - FM_STATUS_H;
        int pyrest = wy + FM_TITLE_H + FM_TOOLBAR_H + FM_BREAD_H + ph - 60;
        int pyemp  = wy + FM_TITLE_H + FM_TOOLBAR_H + FM_BREAD_H + ph - 32;
        if (x >= px && x < px + pw && y >= pyrest && y < pyrest + 22) {
            fm_op_trash_restore(g_fm.focus_idx);
            return 1;
        }
        if (x >= px && x < px + pw && y >= pyemp && y < pyemp + 22) {
            fm_op_trash_purge_all();
            return 1;
        }
    }
    return 1;
}

int file_mgr_mouse_move(int x, int y) { (void)x; (void)y; return 0; }
int file_mgr_mouse_up(int x, int y, int button) {
    (void)x; (void)y; (void)button;
    g_fm.drag_active = 0;
    return 0;
}

void file_mgr_tick(void) {
    if (!g_fm.active) return;
    if (g_fm.dirty_listing) {
        fm_refresh_listing();
        compositor_dirty(g_fm.wx, g_fm.wy, FM_W, FM_H);
    }
}

/* ── Persistence ────────────────────────────────────────────────── */
#define FM_PERSIST_PATH "/file_mgr/state"

static void fm_persist_save_internal(void) {
    (void)vault_mkdir("/file_mgr", VAULT_TIER_INTERNAL);
    char buf[1024];
    int o = 0;
    /* Layout: cwd\twx\twy\n then recents one per line */
    for (int i = 0; g_fm.cwd[i] && o < 1000; i++) buf[o++] = g_fm.cwd[i];
    if (o < 1000) buf[o++] = '\t';
    char ns[16];
    fm_itoa((uint32_t)g_fm.wx, ns);
    for (int i = 0; ns[i] && o < 1000; i++) buf[o++] = ns[i];
    if (o < 1000) buf[o++] = '\t';
    fm_itoa((uint32_t)g_fm.wy, ns);
    for (int i = 0; ns[i] && o < 1000; i++) buf[o++] = ns[i];
    if (o < 1000) buf[o++] = '\n';
    for (int r = 0; r < g_fm.recent_count && o < 1000; r++) {
        for (int i = 0; g_fm.recents[r][i] && o < 1000; i++)
            buf[o++] = g_fm.recents[r][i];
        if (o < 1000) buf[o++] = '\n';
    }
    buf[o] = 0;
    (void)vault_write(FM_PERSIST_PATH, buf, (uint32_t)o);
}

void file_mgr_persist_save(void) { fm_persist_save_internal(); }

void file_mgr_persist_restore(void) {
    char buf[1024];
    int got = vault_read(FM_PERSIST_PATH, buf, sizeof(buf) - 1);
    if (got <= 0) return;
    buf[got] = 0;
    /* First line: cwd \t wx \t wy */
    int i = 0;
    char cwd[FM_PATH_MAX]; int p = 0;
    while (i < got && buf[i] != '\t' && buf[i] != '\n' && p < FM_PATH_MAX-1)
        cwd[p++] = buf[i++];
    cwd[p] = 0;
    int wx = 0, wy = 0;
    if (i < got && buf[i] == '\t') {
        i++;
        while (i < got && buf[i] >= '0' && buf[i] <= '9')
            wx = wx*10 + (buf[i++]-'0');
        if (i < got && buf[i] == '\t') {
            i++;
            while (i < got && buf[i] >= '0' && buf[i] <= '9')
                wy = wy*10 + (buf[i++]-'0');
        }
    }
    while (i < got && buf[i] != '\n') i++;
    if (i < got) i++;
    if (cwd[0]) {
        /* Don't auto-open the window, but stash recents+wx/wy for next open */
        g_fm.wx = wx; g_fm.wy = wy;
    }
    /* Recents: one per line */
    g_fm.recent_count = 0;
    while (i < got && g_fm.recent_count < FM_RECENT_MAX) {
        char r[FM_PATH_MAX]; int rp = 0;
        while (i < got && buf[i] != '\n' && rp < FM_PATH_MAX-1) r[rp++] = buf[i++];
        r[rp] = 0;
        if (i < got) i++;
        if (r[0]) {
            fm_strncpy(g_fm.recents[g_fm.recent_count], r, FM_PATH_MAX);
            g_fm.recent_count++;
        }
    }
}

/* ── Selftest ───────────────────────────────────────────────────── */

void file_mgr_print_selftest_line(void) {
    int chains_ok = 0;
    if (CHAIN_FS_TRASH_REACT >= 0) chains_ok++;
    if (CHAIN_FS_INDEX       >= 0) chains_ok++;
    if (CHAIN_FS_UNDO        >= 0) chains_ok++;
    if (CHAIN_FS_NOTIFY      >= 0) chains_ok++;
    kputs("File manager ......... ");
    kput_dec((uint64_t)chains_ok);
    kputs(" chains (event + trash_react + index + undo + notify), ");
    kput_dec((uint64_t)fs_event_total_emits());
    kputs(" events tracked\n");
}

/* ── Shell command ──────────────────────────────────────────────── */

void file_mgr_cmd(const char *args) {
    while (args && *args == ' ') args++;
    const char *p = (args && *args) ? args : "/";
    if (file_mgr_open(p) != 0) {
        kputs("file_mgr: open failed\n");
        return;
    }
    kputs("file manager opened at ");
    kputs(p);
    kputs("\n");
}
