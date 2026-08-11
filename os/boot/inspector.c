/*
 * Zeos -- Signal Inspector (implementation)
 *
 * Right-side panel showing full chain state.
 * Static state, no malloc, bare-metal.
 */

#include "inspector.h"
#include "chain.h"
#include "mde.h"
#include "b3.h"
#include "fb.h"
#include "font.h"
#include "theme.h"
#include "sigviz.h"
#include "persona_filter.h"
#include "ui_hover.h"
#include "ui_context_menu.h"
#include "kprint.h"

/* ── UI primitive wiring ── */
#define INSPECTOR_MAX_ROWS 8
static uint64_t s_row_tokens[INSPECTOR_MAX_ROWS];
static int      s_row_token_count = 0;
static int      s_rc_chain_id = -1;

/* ── Constants ──────────────────────────────────────────────────── */

#define INSPECTOR_WIDTH    320
#define INSPECTOR_PADDING  Z3      /* 12px internal padding */
#define SECTION_GAP        Z2      /* 8px between sections */
#define LINE_HEIGHT        20      /* px per text line */
#define HEADER_HEIGHT      28      /* px per section header */
#define BETA_BAR_HEIGHT    12      /* px for the Beta distribution bar */
#define BETA_BAR_WIDTH     (INSPECTOR_WIDTH - INSPECTOR_PADDING * 2)
#define MAX_ROUTE_DISPLAY  16      /* max routes shown per direction */

/* ── Static State ───────────────────────────────────────────────── */

static inspector_state_t state;

/* Scratch buffers for number formatting and route queries */
static char fmt_buf[128];
static mde_route_t route_buf[MAX_ROUTE_DISPLAY];

/* ── Helpers ────────────────────────────────────────────────────── */

static void str_copy_local(char *dst, const char *src, int max)
{
    int i = 0;
    while (i < max - 1 && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int str_len(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

/*
 * Format an integer into fmt_buf. Returns pointer to fmt_buf.
 */
static const char *fmt_int(int val)
{
    char tmp[32];
    int i = 0;
    int neg = 0;
    unsigned int uval;

    if (val < 0) {
        neg = 1;
        uval = (unsigned int)(-(val + 1)) + 1;
    } else {
        uval = (unsigned int)val;
    }

    if (uval == 0) {
        tmp[i++] = '0';
    } else {
        while (uval > 0) {
            tmp[i++] = '0' + (char)(uval % 10);
            uval /= 10;
        }
    }

    {
        int pos = 0;
        if (neg) fmt_buf[pos++] = '-';
        while (i > 0) fmt_buf[pos++] = tmp[--i];
        fmt_buf[pos] = '\0';
    }

    return fmt_buf;
}

/*
 * Format a float with 2 decimal places into fmt_buf. Returns pointer.
 */
static const char *fmt_float2(float val)
{
    int whole, frac, pos = 0;
    int neg = 0;

    if (val < 0.0f) {
        neg = 1;
        val = -val;
    }

    whole = (int)val;
    frac = (int)((val - (float)whole) * 100.0f + 0.5f);
    if (frac >= 100) {
        whole++;
        frac -= 100;
    }

    if (neg) fmt_buf[pos++] = '-';

    /* whole part */
    {
        const char *w = fmt_int(whole);
        int wi = 0;
        /* fmt_int wrote to fmt_buf, copy out */
        char wbuf[32];
        while (w[wi]) { wbuf[wi] = w[wi]; wi++; }
        wbuf[wi] = '\0';
        wi = 0;
        while (wbuf[wi]) fmt_buf[pos++] = wbuf[wi++];
    }

    fmt_buf[pos++] = '.';
    fmt_buf[pos++] = '0' + (char)(frac / 10);
    fmt_buf[pos++] = '0' + (char)(frac % 10);
    fmt_buf[pos] = '\0';

    return fmt_buf;
}

/*
 * Format a percentage (float 0.0-1.0 -> "XX%") into fmt_buf.
 */
static const char *fmt_pct(float val)
{
    int pct = (int)(val * 100.0f + 0.5f);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    (void)fmt_int(pct);
    {
        int len = str_len(fmt_buf);
        fmt_buf[len] = '%';
        fmt_buf[len + 1] = '\0';
    }
    return fmt_buf;
}

/*
 * Concatenate strings into a buffer. Variadic not available in
 * freestanding, so we do it manually with a build function.
 */
static int buf_pos;
static char line_buf[256];

static void line_clear(void)
{
    buf_pos = 0;
    line_buf[0] = '\0';
}

static void line_add(const char *s)
{
    int i = 0;
    while (s[i] && buf_pos < 255) {
        line_buf[buf_pos++] = s[i++];
    }
    line_buf[buf_pos] = '\0';
}

/* ── Drawing helpers ────────────────────────────────────────────── */

/*
 * Draw a section header. Returns y after header.
 */
static int draw_header(int x, int y, const char *title)
{
    uint32_t accent = theme_accent();
    fb_text(x, y, title, accent);
    y += HEADER_HEIGHT;
    return y;
}

/*
 * Draw a label: value pair. Returns y after line.
 */
static int draw_label_value(int x, int y, const char *label, const char *value)
{
    int lw;
    fb_text(x, y, label, COLOR_ON_SURFACE_2);
    lw = str_len(label) * 8 + 4;  /* boot font ~8px per char + gap */
    fb_text(x + lw, y, value, COLOR_ON_SURFACE);
    return y + LINE_HEIGHT;
}

/*
 * Draw a separator line. Returns y after separator.
 */
static int draw_separator(int x, int y, int w)
{
    y += SECTION_GAP / 2;
    fb_hline(x, y, w, COLOR_SEPARATOR);
    y += SECTION_GAP / 2 + 1;
    return y;
}

/*
 * Draw a color dot (status/tier indicator).
 */
static void draw_dot(int x, int y, uint32_t color)
{
    fb_circle_filled(x + 4, y + 6, 4, color);
}

/* ── MasQ tier helpers ──────────────────────────────────────────── */

static const char *masq_name(masq_tier_t tier)
{
    switch (tier) {
    case MASQ_SOVEREIGN:  return "Sovereign";
    case MASQ_INTERNAL:   return "Internal";
    case MASQ_REFERENCE:  return "Reference";
    }
    return "Unknown";
}

static uint32_t masq_color(masq_tier_t tier)
{
    switch (tier) {
    case MASQ_SOVEREIGN:  return COLOR_TIER_SOVEREIGN;
    case MASQ_INTERNAL:   return COLOR_TIER_INTERNAL;
    case MASQ_REFERENCE:  return COLOR_TIER_REFERENCE;
    }
    return COLOR_ON_SURFACE_3;
}

/* ── Status helpers ─────────────────────────────────────────────── */

static const char *status_name(chain_status_t s)
{
    switch (s) {
    case CHAIN_LIVE:      return "Live";
    case CHAIN_PAUSED:    return "Paused";
    case CHAIN_ERROR:     return "Error";
    case CHAIN_DETACHED:  return "Detached";
    }
    return "Unknown";
}

static uint32_t status_color(chain_status_t s)
{
    switch (s) {
    case CHAIN_LIVE:      return COLOR_SUCCESS;
    case CHAIN_PAUSED:    return COLOR_WARNING;
    case CHAIN_ERROR:     return COLOR_DANGER;
    case CHAIN_DETACHED:  return COLOR_ON_SURFACE_3;
    }
    return COLOR_ON_SURFACE_3;
}

/* ── Beta distribution bar ──────────────────────────────────────── */

/*
 * Draw a crude bar approximation of the Beta(alpha, beta) shape.
 * We sample the PDF at evenly spaced points across [0,1] and draw
 * vertical bars proportional to the value.
 *
 * Beta PDF: f(x) ~ x^(a-1) * (1-x)^(b-1)
 * We skip the normalizing constant and just scale to fit.
 */
static void draw_beta_bar(int x, int y, int w, int h, float alpha, float beta)
{
    int i;
    float max_val = 0.0f;
    int cols = w;
    uint32_t accent = theme_accent_dim();

    if (cols > 256) cols = 256;

    /* Background */
    fb_rect(x, y, w, h, COLOR_SURFACE);

    /* First pass: find max for normalization */
    for (i = 0; i < cols; i++) {
        float t = ((float)i + 0.5f) / (float)cols;
        float val = 1.0f;
        float a1 = alpha - 1.0f;
        float b1 = beta - 1.0f;
        int j;

        /* x^(a-1): repeated multiplication approximation */
        if (a1 > 0.0f) {
            /* Integer part */
            int ia = (int)a1;
            for (j = 0; j < ia; j++) val *= t;
            /* Fractional part: linear interpolation (crude but no libm) */
            {
                float frac = a1 - (float)ia;
                if (frac > 0.01f) val *= (1.0f - frac) + frac * t;
            }
        } else if (a1 < 0.0f && t > 0.001f) {
            /* a < 1: x^(a-1) = 1/x^(1-a), diverges at 0 */
            float inv = 1.0f / t;
            float ia = (int)(-a1);
            for (j = 0; j < (int)ia; j++) val *= inv;
        }

        /* (1-x)^(b-1) */
        {
            float omt = 1.0f - t;
            if (b1 > 0.0f) {
                int ib = (int)b1;
                for (j = 0; j < ib; j++) val *= omt;
                {
                    float frac = b1 - (float)ib;
                    if (frac > 0.01f) val *= (1.0f - frac) + frac * omt;
                }
            } else if (b1 < 0.0f && omt > 0.001f) {
                float inv = 1.0f / omt;
                int ib = (int)(-b1);
                for (j = 0; j < ib; j++) val *= inv;
            }
        }

        if (val > max_val) max_val = val;
    }

    if (max_val < 0.001f) max_val = 1.0f;

    /* Second pass: draw bars */
    for (i = 0; i < cols; i++) {
        float t = ((float)i + 0.5f) / (float)cols;
        float val = 1.0f;
        float a1 = alpha - 1.0f;
        float b1 = beta - 1.0f;
        int bar_h;
        int j;

        if (a1 > 0.0f) {
            int ia = (int)a1;
            for (j = 0; j < ia; j++) val *= t;
            {
                float frac = a1 - (float)ia;
                if (frac > 0.01f) val *= (1.0f - frac) + frac * t;
            }
        } else if (a1 < 0.0f && t > 0.001f) {
            float inv = 1.0f / t;
            int ia = (int)(-a1);
            for (j = 0; j < (int)ia; j++) val *= inv;
        }

        {
            float omt = 1.0f - t;
            if (b1 > 0.0f) {
                int ib = (int)b1;
                for (j = 0; j < ib; j++) val *= omt;
                {
                    float frac = b1 - (float)ib;
                    if (frac > 0.01f) val *= (1.0f - frac) + frac * omt;
                }
            } else if (b1 < 0.0f && omt > 0.001f) {
                float inv = 1.0f / omt;
                int ib = (int)(-b1);
                for (j = 0; j < ib; j++) val *= inv;
            }
        }

        bar_h = (int)((val / max_val) * (float)(h - 1));
        if (bar_h < 0) bar_h = 0;
        if (bar_h >= h) bar_h = h - 1;

        if (bar_h > 0) {
            fb_vline(x + i, y + h - bar_h, bar_h, accent);
        }
    }

    /* Outline */
    fb_rect_outline(x, y, w, h, COLOR_SEPARATOR, 1);
}

/* ── CFA address formatting ─────────────────────────────────────── */

static const char *fmt_cfa(const cfa_addr_t *addr)
{
    int pos = 0;
    int i;

    for (i = 0; i < (int)addr->depth && i < 8; i++) {
        uint32_t seg = addr->segments[i];
        const char *s;

        fmt_buf[pos++] = '/';

        s = fmt_int((int)seg);
        {
            int j = 0;
            while (s[j] && pos < 126) {
                fmt_buf[pos++] = s[j++];
            }
        }
    }

    if (pos == 0) {
        fmt_buf[pos++] = '/';
    }

    fmt_buf[pos] = '\0';
    return fmt_buf;
}

/* ── API Implementation ─────────────────────────────────────────── */

/* ── Right-click actions ── */
static void rc_resolve_once(void *ctx) {
    (void)ctx;
    if (s_rc_chain_id < 0) return;
    (void)chain_resolve(s_rc_chain_id);
    kputs("INSP: resolve once chain="); kput_dec((uint64_t)s_rc_chain_id); kputs("\n");
}
static void rc_pause(void *ctx) {
    (void)ctx;
    if (s_rc_chain_id < 0) return;
    chain_t *c = chain_get(s_rc_chain_id);
    if (c) {
        c->status = (c->status == CHAIN_PAUSED) ? CHAIN_LIVE : CHAIN_PAUSED;
        kputs("INSP: toggled pause chain="); kput_dec((uint64_t)s_rc_chain_id); kputs("\n");
    }
}
static void rc_reset_b3(void *ctx) {
    (void)ctx;
    if (s_rc_chain_id < 0) return;
    chain_t *c = chain_get(s_rc_chain_id);
    if (c) {
        c->b3_alpha = 1.0f; c->b3_beta = 1.0f; c->b3_observations = 0;
        kputs("INSP: reset B3 chain="); kput_dec((uint64_t)s_rc_chain_id); kputs("\n");
    }
}

int inspector_right_click(int x, int y) {
    extern const inspector_state_t *inspector_get_state(void);
    const inspector_state_t *st = inspector_get_state();
    if (!st->visible) return 0;
    if (x < st->x || x >= st->x + st->w) return 0;
    if (y < st->y || y >= st->y + st->h) return 0;
    s_rc_chain_id = st->target_chain_id;
    static const ctx_menu_item_t items[3] = {
        { "Resolve once", rc_resolve_once, 0, 1 },
        { "Pause",        rc_pause,        0, 1 },
        { "Reset B3",     rc_reset_b3,     0, 1 },
    };
    context_menu_open(x, y, items, 3);
    return 1;
}

void inspector_init(void)
{
    state.visible = 0;
    state.target_chain_id = -1;
    state.x = 0;
    state.y = 0;
    state.w = INSPECTOR_WIDTH;
    state.h = 0;
    state.scroll_y = 0;
}

void inspector_show(int chain_id)
{
    chain_t *c = chain_get(chain_id);
    if (!c) return;

    /* MasQ persona filter: don't show chains beyond perception */
    if (!persona_can_see(chain_id)) return;

    state.visible = 1;
    state.target_chain_id = chain_id;
    state.scroll_y = 0;

    /* Position: right side, full height below toolbar */
    state.w = INSPECTOR_WIDTH;
    state.h = (int)fb_height() - TOOLBAR_HEIGHT;
    state.x = (int)fb_width() - INSPECTOR_WIDTH;
    state.y = TOOLBAR_HEIGHT;
}

void inspector_hide(void)
{
    state.visible = 0;
    state.target_chain_id = -1;
}

void inspector_toggle(int chain_id)
{
    if (state.visible && state.target_chain_id == chain_id) {
        inspector_hide();
    } else {
        inspector_show(chain_id);
    }
}

void inspector_scroll(int delta)
{
    state.scroll_y += delta;
    if (state.scroll_y < 0)
        state.scroll_y = 0;
}

const inspector_state_t *inspector_get_state(void)
{
    return &state;
}

/* ── Draw ───────────────────────────────────────────────────────── */

void inspector_draw(void)
{
    chain_t *c;
    int x, y, inner_w;
    int content_y;
    int i, count;

    if (!state.visible)
        return;

    c = chain_get(state.target_chain_id);
    if (!c) {
        inspector_hide();
        return;
    }

    /* Recalculate position in case screen resized */
    state.h = (int)fb_height() - TOOLBAR_HEIGHT;
    state.x = (int)fb_width() - INSPECTOR_WIDTH;
    state.y = TOOLBAR_HEIGHT;

    /* Refresh hover zones — one per section (5 sections). */
    for (int i = 0; i < s_row_token_count; i++) hover_unregister(s_row_tokens[i]);
    s_row_token_count = 0;
    {
        int row_h = state.h / 5;
        for (int i = 0; i < 5 && s_row_token_count < INSPECTOR_MAX_ROWS; i++) {
            uint64_t tok = hover_register(state.x, state.y + i * row_h,
                                          state.w, row_h, HOVER_CURSOR_POINTER, 0, 0);
            if (tok) s_row_tokens[s_row_token_count++] = tok;
        }
    }

    /* Panel background */
    fb_rect(state.x, state.y, state.w, state.h, COLOR_SURFACE_HIGH);

    /* Left border accent line */
    fb_vline(state.x, state.y, state.h, COLOR_SEPARATOR);

    x = state.x + INSPECTOR_PADDING;
    inner_w = state.w - INSPECTOR_PADDING * 2;
    content_y = state.y + INSPECTOR_PADDING - state.scroll_y;
    y = content_y;

    /* ────────────────────────────────────────────────────────────── */
    /* Section 1: Identity                                            */
    /* ────────────────────────────────────────────────────────────── */

    y = draw_header(x, y, "IDENTITY");

    /* Chain name */
    y = draw_label_value(x, y, "Name: ", c->name);

    /* CFA address */
    {
        /* Format CFA into a local copy so fmt_buf isn't clobbered */
        char cfa_str[128];
        const char *cfa = fmt_cfa(&c->addr);
        str_copy_local(cfa_str, cfa, 128);
        y = draw_label_value(x, y, "CFA: ", cfa_str);
    }

    /* MasQ tier with color dot */
    {
        const char *tn = masq_name(c->tier);
        uint32_t tc = masq_color(c->tier);
        draw_dot(x, y, tc);
        fb_text(x + 14, y, "MasQ: ", COLOR_ON_SURFACE_2);
        fb_text(x + 14 + 6 * 8 + 4, y, tn, tc);
        y += LINE_HEIGHT;
    }

    /* Status with color dot */
    {
        const char *sn = status_name(c->status);
        uint32_t sc = status_color(c->status);
        draw_dot(x, y, sc);
        fb_text(x + 14, y, "Status: ", COLOR_ON_SURFACE_2);
        fb_text(x + 14 + 8 * 8 + 4, y, sn, sc);
        y += LINE_HEIGHT;
    }

    y = draw_separator(x, y, inner_w);

    /* ────────────────────────────────────────────────────────────── */
    /* Section 2: B3 Belief                                           */
    /* ────────────────────────────────────────────────────────────── */

    y = draw_header(x, y, "B3 BELIEF");

    /* Alpha / Beta */
    {
        char ab_str[64];
        const char *a_str;
        const char *b_str;
        int pos = 0;

        a_str = fmt_float2(c->b3_alpha);
        str_copy_local(ab_str, a_str, 32);

        /* Build line manually */
        line_clear();
        line_add("a = ");
        line_add(ab_str);
        line_add(",  b = ");
        b_str = fmt_float2(c->b3_beta);
        line_add(b_str);

        /* Greek letters approximation: use a/b since boot font lacks greek */
        fb_text(x, y, line_buf, COLOR_ON_SURFACE);
        y += LINE_HEIGHT;
    }

    /* Observations */
    y = draw_label_value(x, y, "Observations: ", fmt_int(c->b3_observations));

    /* E[f] = prediction (posterior mean) */
    {
        float ab_sum = c->b3_alpha + c->b3_beta;
        float prediction = 0.5f;
        if (ab_sum > 0.0f)
            prediction = c->b3_alpha / ab_sum;

        line_clear();
        line_add("E[f] = ");
        line_add(fmt_float2(prediction));
        fb_text(x, y, "E[f] = ", COLOR_ON_SURFACE_2);
        fb_text(x + 7 * 8, y, fmt_float2(prediction), COLOR_ON_SURFACE);
        y += LINE_HEIGHT;
    }

    /* Confidence */
    {
        float ab_sum = c->b3_alpha + c->b3_beta;
        float variance = 0.25f;
        float confidence;

        if (ab_sum > 0.0f) {
            variance = (c->b3_alpha * c->b3_beta) /
                       (ab_sum * ab_sum * (ab_sum + 1.0f));
        }
        confidence = 1.0f - variance;
        if (confidence < 0.0f) confidence = 0.0f;
        if (confidence > 1.0f) confidence = 1.0f;

        y = draw_label_value(x, y, "Confidence: ", fmt_pct(confidence));
    }

    /* Beta distribution shape bar */
    y += 2;
    draw_beta_bar(x, y, inner_w, BETA_BAR_HEIGHT, c->b3_alpha, c->b3_beta);
    y += BETA_BAR_HEIGHT + 4;

    y = draw_separator(x, y, inner_w);

    /* ────────────────────────────────────────────────────────────── */
    /* Section 3: Nodes                                               */
    /* ────────────────────────────────────────────────────────────── */

    y = draw_header(x, y, "NODES");

    if (c->node_count == 0) {
        fb_text(x, y, "(no nodes)", COLOR_ON_SURFACE_3);
        y += LINE_HEIGHT;
    } else {
        for (i = 0; i < c->node_count; i++) {
            chain_node_t *node = &c->nodes[i];
            uint32_t node_color = COLOR_ON_SURFACE;

            /* Node index and name */
            line_clear();
            line_add("[");
            line_add(fmt_int(i));
            line_add("] ");
            line_add(node->name);
            fb_text(x, y, line_buf, node_color);
            y += LINE_HEIGHT;

            /* Input -> Output type mapping */
            line_clear();
            line_add("    ");
            line_add(node->input_type);
            line_add(" -> ");
            line_add(node->output_type);
            fb_text(x, y, line_buf, COLOR_ON_SURFACE_2);
            y += LINE_HEIGHT;
        }
    }

    y = draw_separator(x, y, inner_w);

    /* ────────────────────────────────────────────────────────────── */
    /* Section 4: Routes                                              */
    /* ────────────────────────────────────────────────────────────── */

    y = draw_header(x, y, "ROUTES");

    /* Incoming routes */
    fb_text(x, y, "Incoming:", COLOR_ON_SURFACE_2);
    y += LINE_HEIGHT;

    count = mde_find_routes_to(state.target_chain_id, route_buf, MAX_ROUTE_DISPLAY);
    if (count == 0) {
        fb_text(x + 8, y, "(none)", COLOR_ON_SURFACE_3);
        y += LINE_HEIGHT;
    } else {
        for (i = 0; i < count; i++) {
            chain_t *from = chain_get(route_buf[i].from_chain);
            line_clear();
            line_add("  ");
            if (from) {
                line_add(from->name);
            } else {
                line_add("chain#");
                line_add(fmt_int(route_buf[i].from_chain));
            }
            line_add(" [");
            line_add(route_buf[i].signal_type);
            line_add("]");
            fb_text(x, y, line_buf, COLOR_ON_SURFACE);
            y += LINE_HEIGHT;
        }
    }

    y += 4;

    /* Outgoing routes */
    fb_text(x, y, "Outgoing:", COLOR_ON_SURFACE_2);
    y += LINE_HEIGHT;

    count = mde_find_routes_from(state.target_chain_id, route_buf, MAX_ROUTE_DISPLAY);
    if (count == 0) {
        fb_text(x + 8, y, "(none)", COLOR_ON_SURFACE_3);
        y += LINE_HEIGHT;
    } else {
        for (i = 0; i < count; i++) {
            chain_t *to = chain_get(route_buf[i].to_chain);
            line_clear();
            line_add("  ");
            if (to) {
                line_add(to->name);
            } else {
                line_add("chain#");
                line_add(fmt_int(route_buf[i].to_chain));
            }
            line_add(" [");
            line_add(route_buf[i].signal_type);
            line_add("]");
            fb_text(x, y, line_buf, COLOR_ON_SURFACE);
            y += LINE_HEIGHT;
        }
    }

    y = draw_separator(x, y, inner_w);

    /* ────────────────────────────────────────────────────────────── */
    /* Section 5: Timing                                              */
    /* ────────────────────────────────────────────────────────────── */

    y = draw_header(x, y, "TIMING");

    /* Last resolve */
    {
        char ms_str[32];
        str_copy_local(ms_str, fmt_float2(c->last_resolve_ms), 32);
        line_clear();
        line_add(ms_str);
        line_add(" ms");
        y = draw_label_value(x, y, "Last resolve: ", line_buf);
    }

    /* Created TSC */
    {
        /* Show as raw TSC count -- no wall clock in bare metal */
        char tsc_str[32];
        uint64_t tsc = c->created_tsc;
        int pos = 0;

        /* Simple decimal formatting for uint64 */
        if (tsc == 0) {
            tsc_str[0] = '0';
            tsc_str[1] = '\0';
        } else {
            char tmp[24];
            int ti = 0;
            while (tsc > 0) {
                tmp[ti++] = '0' + (char)(tsc % 10);
                tsc /= 10;
            }
            while (ti > 0) tsc_str[pos++] = tmp[--ti];
            tsc_str[pos] = '\0';
        }

        y = draw_label_value(x, y, "Created: ", tsc_str);
    }

    /* VAULT version */
    y = draw_label_value(x, y, "VAULT version: ", fmt_int(c->vault_version));
}
