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
#include "compositor.h"
#include "ui_states.h"
#include "kprint.h"
#include "notify.h"

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
} bp_state_t;

static bp_state_t B;

static int  s_len(const char *s){int n=0;if(!s)return 0;while(s[n])n++;return n;}
static void s_cpy(char *d,const char *s,int max){int i=0;if(!d||max<=0)return;if(s)while(i<max-1&&s[i]){d[i]=s[i];i++;}d[i]=0;}
static void s_itoa(uint64_t v,char *o){char b[24];int n=0;if(!v){o[0]='0';o[1]=0;return;}while(v){b[n++]=(char)('0'+(v%10));v/=10;}int k=0;while(n)o[k++]=b[--n];o[k]=0;}

void build_polish_init(void) {
    if (B.initialized) return;
    B.initialized = 1;
    B.surface_id = -1;
    B.focus_rule = -1;
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
    /* 6-wide grid; deterministic and stable across redraws. */
    int cols = 6;
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
