/*
 * build-zeos polish — DAG visualization for the chain registry as a build
 * system. Each rule is a circle; edges are dependencies. Color encodes
 * state: pending grey / running blue / done green / failed red.
 *
 * The progress bar across the top shows finished/total. The active
 * rule's stdout/stderr appears in the side pane. Time-budget at bottom.
 */

#include "build_polish.h"
#include "fb.h"
#include "font.h"
#include "theme.h"
#include "wm.h"
#include "wm_persist.h"
#include "compositor.h"
#include "ui_states.h"
#include "ui_context_menu.h"
#include "kprint.h"
#include "notify.h"
#include "settings_registry.h"
#include "smp.h"

#define BP_W           1100
#define BP_H            700
#define BP_MAX_RULES     32
#define BP_NAME_MAX      48
#define BP_PATH_MAX      96
#define BP_CMD_MAX      192
#define BP_STDERR_RING 4096

typedef struct {
    int           used;
    int           id;
    char          name[BP_NAME_MAX];
    char          inputs[BP_PATH_MAX];
    char          output[BP_PATH_MAX];
    char          cmd[BP_CMD_MAX];
    build_state_t state;
    uint32_t      took_ms;
    /* DAG layout: deterministic from id (concentric layers). */
    int           layout_x, layout_y;
    /* stderr ring */
    char          stderr_buf[BP_STDERR_RING];
    int           stderr_head;
    int           stderr_len;
} bp_rule_t;

typedef struct {
    int        active, initialized;
    int        surface_id;
    bp_rule_t  rules[BP_MAX_RULES];
    int        rule_count;
    int        focus_rule;          /* -1 = none */
    uint32_t   last_build_ms;
    uint32_t   prev_build_ms;
    /* View state. */
    int        sort_col;
    int        sort_dir;
    int        theme_idx;
    int        scroll_pos;
    /* Tunables. */
    int        parallelism;
    int        fail_fast;
    int        dag_grid_width;
} bp_state_t;

static bp_state_t B;

static int  s_len(const char *s){int n=0;if(!s)return 0;while(s[n])n++;return n;}
static void s_cpy(char *d,const char *s,int max){int i=0;if(!d||max<=0)return;if(s)while(i<max-1&&s[i]){d[i]=s[i];i++;}d[i]=0;}
static void s_itoa(uint64_t v,char *o){char b[24];int n=0;if(!v){o[0]='0';o[1]=0;return;}while(v){b[n++]=(char)('0'+(v%10));v/=10;}int k=0;while(n)o[k++]=b[--n];o[k]=0;}

/* Forward decls. */
static void bp_get_state(wm_persist_blob_t *out);
static void bp_apply_state(const wm_persist_blob_t *in);
static void bp_on_right_click(int local_x, int local_y);
static void bp_register_settings(void);

void build_polish_init(void) {
    if (B.initialized) return;
    B.initialized = 1;
    B.surface_id = -1;
    B.focus_rule = -1;
    B.parallelism = 4;
    B.fail_fast = 0;
    B.dag_grid_width = 6;
    wm_persist_register("build-zeos", bp_get_state, bp_apply_state);
    bp_register_settings();
}

int build_polish_rule_add(const char *name, const char *inputs,
                          const char *output, const char *cmd) {
    build_polish_init();
    for (int i = 0; i < BP_MAX_RULES; i++) {
        if (!B.rules[i].used) {
            B.rules[i].used = 1;
            B.rules[i].id = i;
            s_cpy(B.rules[i].name,   name,   BP_NAME_MAX);
            s_cpy(B.rules[i].inputs, inputs, BP_PATH_MAX);
            s_cpy(B.rules[i].output, output, BP_PATH_MAX);
            s_cpy(B.rules[i].cmd,    cmd,    BP_CMD_MAX);
            B.rules[i].state = BP_PENDING;
            B.rule_count++;
            return i;
        }
    }
    return -1;
}

void build_polish_rule_state(int id, build_state_t s, uint32_t took_ms) {
    if (id < 0 || id >= BP_MAX_RULES || !B.rules[id].used) return;
    build_state_t prev = B.rules[id].state;
    B.rules[id].state = s;
    B.rules[id].took_ms = took_ms;
    if (s == BP_FAILED && prev != BP_FAILED) {
        notify_send(B.rules[id].name, "build-zeos", NOTIFY_ERROR);
    } else if (s == BP_DONE && prev != BP_DONE) {
        B.last_build_ms += took_ms;
        /* No success toast for individual rules — too noisy. */
    }
}

void build_polish_rule_stderr(int id, const char *line) {
    if (id < 0 || id >= BP_MAX_RULES || !B.rules[id].used || !line) return;
    bp_rule_t *r = &B.rules[id];
    int n = s_len(line);
    for (int i = 0; i < n; i++) {
        r->stderr_buf[r->stderr_head] = line[i];
        r->stderr_head = (r->stderr_head + 1) % BP_STDERR_RING;
        if (r->stderr_len < BP_STDERR_RING) r->stderr_len++;
    }
    r->stderr_buf[r->stderr_head] = '\n';
    r->stderr_head = (r->stderr_head + 1) % BP_STDERR_RING;
    if (r->stderr_len < BP_STDERR_RING) r->stderr_len++;
}

int build_polish_rule_count(void) { return B.rule_count; }

/* ── Drawing ───────────────────────────────────────────────────── */
static uint32_t state_color(build_state_t s) {
    switch (s) {
        case BP_PENDING: return COLOR_ON_SURFACE_3;
        case BP_RUNNING: return COLOR_PRIMARY;
        case BP_DONE:    return COLOR_SUCCESS;
        case BP_FAILED:  return COLOR_DANGER;
    }
    return COLOR_ON_SURFACE_3;
}

static void layout_rules(int x, int y, int w, int h) {
    /* Grid width from settings (build.dag_grid_width). */
    int cols = B.dag_grid_width > 0 ? B.dag_grid_width : 6;
    if (cols > 12) cols = 12;
    int cw = w / cols;
    int rows = (B.rule_count + cols - 1) / cols;
    if (rows < 1) rows = 1;
    int rh = h / (rows + 1);
    int idx = 0;
    for (int i = 0; i < BP_MAX_RULES; i++) {
        if (!B.rules[i].used) continue;
        int c = idx % cols;
        int r = idx / cols;
        B.rules[i].layout_x = x + c * cw + cw / 2;
        B.rules[i].layout_y = y + r * rh + rh / 2;
        idx++;
    }
}

static void bp_draw_dag(int x, int y, int w, int h) {
    fb_rect(x, y, w, h, COLOR_SURFACE);
    if (B.rule_count == 0) {
        list_state_ctx_t lc = {.x = x, .y = y, .w = w, .h = h,
                               .state = LIST_EMPTY,
                               .message = "No build rules registered",
                               .cta = "Add one via the chain registry"};
        list_render_state(&lc);
        return;
    }
    layout_rules(x + 40, y + 40, w - 80, h - 80);
    /* Edges: parse inputs (CSV) and find matching outputs. */
    for (int i = 0; i < BP_MAX_RULES; i++) {
        if (!B.rules[i].used) continue;
        const char *p = B.rules[i].inputs;
        while (*p) {
            const char *q = p;
            while (*q && *q != ',') q++;
            int n = q - p;
            char tok[BP_PATH_MAX];
            int j = 0; while (j < n && j < BP_PATH_MAX-1) { tok[j] = p[j]; j++; } tok[j] = 0;
            for (int k = 0; k < BP_MAX_RULES; k++) {
                if (!B.rules[k].used || k == i) continue;
                int eq = 1;
                for (int m = 0; tok[m] || B.rules[k].output[m]; m++) {
                    if (tok[m] != B.rules[k].output[m]) { eq = 0; break; }
                }
                if (eq) {
                    fb_line(B.rules[k].layout_x, B.rules[k].layout_y,
                            B.rules[i].layout_x, B.rules[i].layout_y,
                            COLOR_SEPARATOR);
                }
            }
            p = (*q == ',') ? q + 1 : q;
        }
    }
    /* Nodes */
    for (int i = 0; i < BP_MAX_RULES; i++) {
        if (!B.rules[i].used) continue;
        uint32_t c = state_color(B.rules[i].state);
        fb_circle_filled(B.rules[i].layout_x, B.rules[i].layout_y, 16, c);
        fb_circle(B.rules[i].layout_x, B.rules[i].layout_y, 16, COLOR_ON_SURFACE);
        font_draw(B.rules[i].layout_x - 36, B.rules[i].layout_y + 22,
                  B.rules[i].name, FONT_UI, TYPE_CAPTION, COLOR_ON_SURFACE_2);
    }
}

static void bp_draw_progress(int x, int y, int w) {
    int total = 0, done = 0, running = 0, failed = 0;
    for (int i = 0; i < BP_MAX_RULES; i++) {
        if (!B.rules[i].used) continue;
        total++;
        if (B.rules[i].state == BP_DONE)    done++;
        if (B.rules[i].state == BP_RUNNING) running++;
        if (B.rules[i].state == BP_FAILED)  failed++;
    }
    fb_rect(x, y, w, 8, COLOR_SURFACE_HIGH);
    if (total > 0) {
        int filled = (w * done) / total;
        fb_rect(x, y, filled, 8, failed ? COLOR_DANGER : COLOR_SUCCESS);
    }
    char buf[96]; s_cpy(buf, "rules ", sizeof(buf));
    char num[16]; s_itoa((uint64_t)done, num);
    int n = s_len(buf), j = 0; while (num[j] && n < 95) buf[n++] = num[j++];
    buf[n++] = '/';
    s_itoa((uint64_t)total, num); j = 0; while (num[j] && n < 95) buf[n++] = num[j++];
    s_cpy(buf + n, "    running: ", sizeof(buf) - n); n = s_len(buf);
    s_itoa((uint64_t)running, num); j = 0; while (num[j] && n < 95) buf[n++] = num[j++];
    s_cpy(buf + n, "    failed: ", sizeof(buf) - n); n = s_len(buf);
    s_itoa((uint64_t)failed, num); j = 0; while (num[j] && n < 95) buf[n++] = num[j++]; buf[n] = 0;
    font_draw(x + 4, y + 12, buf, FONT_UI, TYPE_CAPTION, COLOR_ON_SURFACE_2);
}

static void bp_draw_detail(int x, int y, int w, int h) {
    fb_rect(x, y, w, h, COLOR_SURFACE_HIGH);
    fb_rect(x, y, 1, h, COLOR_SEPARATOR);
    int id = B.focus_rule;
    if (id < 0 || id >= BP_MAX_RULES || !B.rules[id].used) {
        font_draw(x + 12, y + 12, "Click a rule to see details",
                  FONT_UI, TYPE_LABEL, COLOR_ON_SURFACE_3);
        return;
    }
    bp_rule_t *r = &B.rules[id];
    font_draw(x + 12, y + 8,  r->name, FONT_UI_BOLD, TYPE_HEADING, COLOR_ON_SURFACE);
    font_draw(x + 12, y + 36, "input:",  FONT_UI, TYPE_CAPTION, COLOR_ON_SURFACE_3);
    font_draw(x + 80, y + 36, r->inputs, FONT_CODE, TYPE_CAPTION, COLOR_ON_SURFACE);
    font_draw(x + 12, y + 54, "output:", FONT_UI, TYPE_CAPTION, COLOR_ON_SURFACE_3);
    font_draw(x + 80, y + 54, r->output, FONT_CODE, TYPE_CAPTION, COLOR_ON_SURFACE);
    font_draw(x + 12, y + 72, "cmd:",    FONT_UI, TYPE_CAPTION, COLOR_ON_SURFACE_3);
    font_draw(x + 80, y + 72, r->cmd,    FONT_CODE, TYPE_CAPTION, COLOR_ON_SURFACE);
    char took[32]; s_itoa((uint64_t)r->took_ms, took);
    font_draw(x + 12, y + 90, "took:",   FONT_UI, TYPE_CAPTION, COLOR_ON_SURFACE_3);
    font_draw(x + 80, y + 90, took,      FONT_CODE, TYPE_CAPTION, COLOR_ON_SURFACE);
    /* stderr ring */
    fb_rect(x + 12, y + 116, w - 24, h - 130, COLOR_SURFACE);
    font_draw(x + 12, y + 110, "stderr ring", FONT_UI_BOLD, TYPE_CAPTION, COLOR_ON_SURFACE_2);
    /* Walk the ring linearly from oldest to newest. */
    int line_y = y + 124;
    int len = r->stderr_len;
    int start = (r->stderr_head - len + BP_STDERR_RING) % BP_STDERR_RING;
    char line[120]; int li = 0;
    for (int i = 0; i < len && line_y < y + h - 16; i++) {
        char c = r->stderr_buf[(start + i) % BP_STDERR_RING];
        if (c == '\n' || li >= 119) {
            line[li] = 0;
            font_draw(x + 14, line_y, line, FONT_CODE, TYPE_CAPTION,
                      r->state == BP_FAILED ? COLOR_DANGER : COLOR_ON_SURFACE_2);
            line_y += 14; li = 0;
        } else {
            line[li++] = c;
        }
    }
}

static void bp_draw_content(int id, int x, int y, int w, int h) {
    (void)id;
    fb_rect(x, y, w, h, COLOR_SURFACE);
    bp_draw_progress(x + 12, y + 8, w - 24);
    int detail_w = 320;
    bp_draw_dag   (x, y + 36, w - detail_w, h - 36);
    bp_draw_detail(x + w - detail_w, y + 36, detail_w, h - 36);
}

void build_polish_open(void) {
    build_polish_init();
    if (B.active) { if (B.surface_id >= 0) wm_focus_surface(B.surface_id); return; }
    int sw = (int)fb_width(), sh = (int)fb_height();
    int sx = (sw - BP_W) / 2, sy = (sh - BP_H) / 2;
    B.surface_id = wm_create_surface("bview — build-zeos DAG", -1,
                                     sx, sy, BP_W, BP_H, bp_draw_content);
    if (B.surface_id < 0) return;
    wm_focus_surface(B.surface_id);
    wm_set_right_click(B.surface_id, bp_on_right_click);
    wm_set_persist_name(B.surface_id, "build-zeos");
    B.active = 1;
}
void build_polish_close(void) {
    if (!B.active) return;
    if (B.surface_id >= 0) wm_detach_surface(B.surface_id);
    B.surface_id = -1;
    B.active = 0;
}
int build_polish_active(void) { return B.active; }

void build_polish_print_selftest_line(void) {
    build_polish_init();
    kputs("build-zeos polish .... DAG view ready, ");
    kput_dec((uint64_t)B.rule_count);
    kputs(" rules registered\n");
}

void build_polish_cmd(const char *args) {
    (void)args;
    build_polish_open();
    kputs("bview opened\n");
}

/* ── Persistence ──────────────────────────────────────────────── */
static void bp_get_state(wm_persist_blob_t *out) {
    chain_surface_t *s = (B.surface_id >= 0) ? wm_get_surface(B.surface_id) : 0;
    if (s) { out->x = s->x; out->y = s->y; out->w = s->w; out->h = s->h; }
    out->sort_col = B.sort_col; out->sort_dir = B.sort_dir;
    out->theme_idx = B.theme_idx; out->scroll_pos = B.scroll_pos;
    out->filter_str[0] = 0;
}
static void bp_apply_state(const wm_persist_blob_t *in) {
    B.sort_col = in->sort_col; B.sort_dir = in->sort_dir;
    B.theme_idx = in->theme_idx; B.scroll_pos = in->scroll_pos;
    if (B.surface_id >= 0 && in->w > 0 && in->h > 0) {
        wm_move_surface(B.surface_id, in->x, in->y);
        wm_resize_surface(B.surface_id, in->w, in->h);
    }
}

/* ── Settings ─────────────────────────────────────────────────── */
static int bp_parse_int(const char *v, int *o){
    if(!v||!*v)return -1; int n=0;
    while(*v){ if(*v<'0'||*v>'9')return -1; n=n*10+(*v-'0'); v++; }
    *o=n; return 0;
}
static void bp_int_emit(int v, char *o, int max){
    char t[12]; int ti=0; int k=0;
    if(v==0)t[ti++]='0';
    while(v>0&&ti<11){ t[ti++]=(char)('0'+v%10); v/=10; }
    while(ti>0&&k<max-1) o[k++]=t[--ti]; o[k]=0;
}
static int bp_g_par(char *o, int m){
    /* Always emit the live (clamped) value, not the raw setting. */
    int cpu = smp_cpu_count(); if (cpu < 1) cpu = 1;
    int v = B.parallelism; if (v > cpu) v = cpu; if (v < 1) v = 1;
    bp_int_emit(v, o, m); return 0;
}
static int bp_s_par(const char *v){
    int n; if (bp_parse_int(v, &n) < 0) return -1;
    if (n < 1) return -1;
    int cpu = smp_cpu_count(); if (cpu < 1) cpu = 1;
    if (n > cpu) n = cpu;        /* clamp */
    B.parallelism = n; return 0;
}
static int bp_g_ff(char *o, int m){ if(m<=0)return 0; o[0]=B.fail_fast?'1':'0'; o[1]=0; return 0; }
static int bp_s_ff(const char *v){
    if(!v)return -1;
    if(v[0]=='0'||v[0]=='f'||v[0]=='F') B.fail_fast=0;
    else if(v[0]=='1'||v[0]=='t'||v[0]=='T') B.fail_fast=1;
    else return -1;
    return 0;
}
static int bp_g_grid(char *o, int m){ bp_int_emit(B.dag_grid_width, o, m); return 0; }
static int bp_s_grid(const char *v){
    int n; if(bp_parse_int(v,&n)<0||n<1||n>12) return -1; B.dag_grid_width=n; return 0;
}
static const settings_entry_t E_BUILD_PAR = {
    .name="build.parallelism",.kind=SK_INT,.flags=0,
    .getter=bp_g_par,.setter=bp_s_par,.enum_labels=0,
    .desc="Parallel rule resolvers (clamped to cpu_count)"};
static const settings_entry_t E_BUILD_FF = {
    .name="build.fail_fast",.kind=SK_BOOL,.flags=0,
    .getter=bp_g_ff,.setter=bp_s_ff,.enum_labels=0,
    .desc="Stop scheduling new rules after the first failure"};
static const settings_entry_t E_BUILD_GRID = {
    .name="build.dag_grid_width",.kind=SK_INT,.flags=0,
    .getter=bp_g_grid,.setter=bp_s_grid,.enum_labels=0,
    .desc="DAG nodes per row in the layout grid"};
static void bp_register_settings(void) {
    (void)settings_register(&E_BUILD_PAR);
    (void)settings_register(&E_BUILD_FF);
    (void)settings_register(&E_BUILD_GRID);
}

/* ── Right-click ──────────────────────────────────────────────── */
static int bp_pick_node(int sx_local, int sy_local) {
    /* Match the geometry used in bp_draw_content. */
    chain_surface_t *s = (B.surface_id >= 0) ? wm_get_surface(B.surface_id) : 0;
    if (!s) return -1;
    int detail_w = 320;
    int dag_w = (s->w - 2) - detail_w;
    int dag_h = (s->h - WM_TITLEBAR_HEIGHT - 2) - 36;
    if (sx_local < 0 || sx_local >= dag_w) return -1;
    if (sy_local < 36 || sy_local >= 36 + dag_h) return -1;
    /* layout_rules uses x+40 / y+40, w-80/h-80 for the inner. The node
     * positions live in B.rules[i].layout_x/y in screen coords because
     * we passed (cx + 40, cy + 40) — but layout_x/y was set at last
     * draw with the same x base. We accept a hit-test on the latest
     * layout values; tolerance 18 (radius+2). */
    /* Convert local to "draw call" coords for that pane: dag pane is
     * at content (cx, cy+36) with size (dag_w, dag_h). layout_rules
     * stored screen coords. Recover screen point: */
    int cx = s->x + 1;
    int cy = s->y + WM_TITLEBAR_HEIGHT + 1;
    int sx = cx + sx_local;
    int sy = cy + sy_local;
    int best = -1; int best_dist = 18 * 18;
    for (int i = 0; i < BP_MAX_RULES; i++) {
        if (!B.rules[i].used) continue;
        int dx = sx - B.rules[i].layout_x;
        int dy = sy - B.rules[i].layout_y;
        int d = dx*dx + dy*dy;
        if (d < best_dist) { best_dist = d; best = i; }
    }
    return best;
}
static void bp_act_run(void *c){ (void)c;
    if (B.focus_rule < 0) return;
    build_polish_rule_state(B.focus_rule, BP_RUNNING, 0);
    notify_send(B.rules[B.focus_rule].name, "rule running", NOTIFY_INFO);
}
static void bp_act_stderr(void *c){ (void)c;
    if (B.focus_rule < 0) return;
    /* Just bring the detail pane into focus on this rule. */
    notify_send(B.rules[B.focus_rule].name, "viewing stderr", NOTIFY_INFO);
}
static void bp_act_force(void *c){ (void)c;
    if (B.focus_rule < 0) return;
    B.rules[B.focus_rule].state = BP_PENDING;
    B.rules[B.focus_rule].took_ms = 0;
    build_polish_rule_state(B.focus_rule, BP_RUNNING, 0);
}
static void bp_act_skip(void *c){ (void)c;
    if (B.focus_rule < 0) return;
    build_polish_rule_state(B.focus_rule, BP_DONE, 0);
}

static void bp_on_right_click(int local_x, int local_y) {
    int id = bp_pick_node(local_x, local_y);
    if (id < 0) return;
    B.focus_rule = id;
    chain_surface_t *s = wm_get_surface(B.surface_id);
    if (!s) return;
    int sx = s->x + 1 + local_x;
    int sy = s->y + WM_TITLEBAR_HEIGHT + 1 + local_y;
    ctx_menu_item_t items[4];
    s_cpy(items[0].label, "Run",          CTX_MENU_LABEL_MAX);
    items[0].action = bp_act_run;    items[0].ctx = 0; items[0].enabled = 1;
    s_cpy(items[1].label, "View stderr", CTX_MENU_LABEL_MAX);
    items[1].action = bp_act_stderr; items[1].ctx = 0; items[1].enabled = 1;
    s_cpy(items[2].label, "Force rebuild", CTX_MENU_LABEL_MAX);
    items[2].action = bp_act_force;  items[2].ctx = 0; items[2].enabled = 1;
    s_cpy(items[3].label, "Skip",         CTX_MENU_LABEL_MAX);
    items[3].action = bp_act_skip;   items[3].ctx = 0; items[3].enabled = 1;
    context_menu_open(sx, sy, items, 4);
}
