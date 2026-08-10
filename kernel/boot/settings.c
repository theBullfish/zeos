/*
 * Zeos -- Settings App
 *
 * System preferences rendered in a WM chain surface.
 * Left sidebar for page navigation, right pane for settings.
 * Arrow keys navigate, Enter cycles values.
 * All changes apply immediately and persist to VAULT.
 */

#include "settings.h"
#include "fb.h"
#include "font.h"
#include "theme.h"
#include "wm.h"
#include "vault.h"
#include "persona.h"
#include "net.h"
#include "net_dhcp.h"
#include "timer.h"
#include "chain.h"

/* ── External state (defined in shell.c) ── */
extern uint32_t theme_accent(void);
extern uint32_t theme_accent_dim(void);

/* ── Layout constants ── */
#define SIDEBAR_W        160
#define ROW_HEIGHT        36
#define PAGE_PADDING      Z4
#define LABEL_X           (SIDEBAR_W + Z6)
#define VALUE_RIGHT_PAD   Z6

/* J.2: color_scheme_t (SCHEME_DARK/LIGHT/AUTO) comes from access.h now (was a
 * duplicate here). Same values, one definition. */

/* ── Static state ── */
static settings_state_t g_settings;
static int g_open = 0;

/* ── Persisted preferences ── */
static int             g_persona       = 2;   /* 0=Zeros, 1=DereZ, 2=Full */
static int             g_controls_side = 0;   /* 0=left, 1=right */
static color_scheme_t  g_color_scheme  = SCHEME_DARK;
static uint32_t        g_wallpaper     = COLOR_SURFACE;

static int             g_mouse_speed   = 5;   /* 1-10 */
static int             g_key_repeat    = 30;  /* ms between repeats */

/* J.2: no private access copy -- the settings GUI operates on the REAL access
 * config (access_get()) and mutates it through access_set_* so changes apply
 * live and persist via access.c. access.h maps anim_speed as a float, so the UI
 * maps its 4 speed steps to/from {0.0, 0.5, 1.0, 2.0}. */
static const float ANIM_SPEED_STEPS[4] = { 0.0f, 0.5f, 1.0f, 2.0f };

static int anim_speed_to_idx(float s)
{
    if (s < 0.25f) return 0;
    if (s < 0.75f) return 1;
    if (s < 1.5f)  return 2;
    return 3;
}

/* ── Page names ── */
static const char *page_names[SETTINGS_PAGE_COUNT] = {
    "Display",
    "Input",
    "Accessibility",
    "Network",
    "About",
};

/* ── Items per page ── */
static const int items_per_page[SETTINGS_PAGE_COUNT] = {
    4,  /* Display: persona, controls, scheme, wallpaper */
    2,  /* Input: mouse speed, key repeat */
    7,  /* Accessibility: sensory, density, reduced motion, anim speed, letter spacing, focus, night shift */
    4,  /* Network: IP, gateway, DNS, DHCP (read-only) */
    5,  /* About: version, tagline, chains, uptime, build */
};

/* ── VAULT persistence keys ── */
#define VKEY_PERSONA       "settings.persona"
#define VKEY_CONTROLS      "settings.controls_side"
#define VKEY_SCHEME        "settings.color_scheme"
#define VKEY_WALLPAPER     "settings.wallpaper"
#define VKEY_MOUSE_SPEED   "settings.mouse_speed"
#define VKEY_KEY_REPEAT    "settings.key_repeat"
#define VKEY_ACCESS        "settings.access"

/* ── Forward declarations ── */
static void draw_sidebar(int x, int y, int h);
static void draw_page_display(int x, int y, int w, int h);
static void draw_page_input(int x, int y, int w, int h);
static void draw_page_accessibility(int x, int y, int w, int h);
static void draw_page_network(int x, int y, int w, int h);
static void draw_page_about(int x, int y, int w, int h);
static void save_all(void);
static void load_all(void);
static void apply_persona(void);
static void apply_controls(void);

/* ── Helpers ── */

static int clamp(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/*
 * Draw a row: label on the left (secondary), value on the right (primary).
 * If this is the selected item, draw an accent underline.
 */
static void draw_row(int x, int y, int w, int row, const char *label,
                     const char *value, int selected)
{
    int ry = y + row * ROW_HEIGHT;
    uint32_t label_color = (COLOR_ON_SURFACE & 0x00FFFFFF) | ((uint32_t)TEXT_SECONDARY << 24);
    uint32_t value_color = (COLOR_ON_SURFACE & 0x00FFFFFF) | ((uint32_t)TEXT_PRIMARY << 24);

    font_draw(x, ry + 8, label, FONT_UI, TYPE_BODY, label_color);

    int vw = font_measure(value, FONT_UI, TYPE_BODY);
    font_draw(x + w - VALUE_RIGHT_PAD - vw, ry + 8, value, FONT_UI, TYPE_BODY, value_color);

    if (selected) {
        fb_rect(x, ry + ROW_HEIGHT - 3, w, 2, theme_accent());
    }
}

/*
 * Draw radio-style options: [Option1] [Option2] [Option3]
 * selected_idx gets accent background.
 */
static void draw_radio(int rx, int ry, const char *options[], int count,
                       int selected_idx)
{
    int cx = rx;
    for (int i = 0; i < count; i++) {
        int tw = font_measure(options[i], FONT_UI, TYPE_LABEL);
        int bw = tw + Z4 * 2;

        if (i == selected_idx) {
            fb_rect(cx, ry, bw, CONTROL_HEIGHT_SM, theme_accent());
            font_draw(cx + Z4, ry + 4, options[i], FONT_UI, TYPE_LABEL, COLOR_SURFACE);
        } else {
            fb_rect_outline(cx, ry, bw, CONTROL_HEIGHT_SM, COLOR_ON_SURFACE_3, 1);
            font_draw(cx + Z4, ry + 4, options[i], FONT_UI, TYPE_LABEL, COLOR_ON_SURFACE_2);
        }
        cx += bw + Z2;
    }
}

/*
 * Draw a row with inline radio buttons instead of a text value.
 */
static void draw_row_radio(int x, int y, int w, int row, const char *label,
                           const char *options[], int count, int selected_idx,
                           int is_selected)
{
    int ry = y + row * ROW_HEIGHT;
    uint32_t label_color = (COLOR_ON_SURFACE & 0x00FFFFFF) | ((uint32_t)TEXT_SECONDARY << 24);

    font_draw(x, ry + 8, label, FONT_UI, TYPE_BODY, label_color);

    /* Radio buttons on the right half */
    int radio_x = x + (w / 2);
    draw_radio(radio_x, ry + 4, options, count, selected_idx);

    if (is_selected) {
        fb_rect(x, ry + ROW_HEIGHT - 3, w, 2, theme_accent());
    }
}

/* ── Number to string (tiny, no sprintf) ── */

static char *int_to_str(int v, char *buf, int bufsz)
{
    if (bufsz < 2) { buf[0] = '\0'; return buf; }
    int neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    int i = bufsz - 1;
    buf[i] = '\0';
    do {
        buf[--i] = '0' + (v % 10);
        v /= 10;
    } while (v && i > 0);
    if (neg && i > 0) buf[--i] = '-';
    return &buf[i];
}

static void ip_to_str(struct ipv4_addr a, char *buf, int bufsz)
{
    /* Manual: a.b.c.d */
    char tmp[4];
    int pos = 0;
    for (int i = 0; i < 4; i++) {
        char *s = int_to_str(a.b[i], tmp, sizeof(tmp));
        while (*s && pos < bufsz - 1) buf[pos++] = *s++;
        if (i < 3 && pos < bufsz - 1) buf[pos++] = '.';
    }
    buf[pos] = '\0';
}

static void hex_to_str(uint32_t v, char *buf, int bufsz)
{
    static const char hex[] = "0123456789ABCDEF";
    if (bufsz < 10) { buf[0] = '\0'; return; }
    buf[0] = '#';
    for (int i = 5; i >= 0; i--) {
        buf[6 - i] = hex[(v >> (i * 4)) & 0xF];
    }
    buf[7] = '\0';
}

/* ── VAULT persistence ── */

static void save_all(void)
{
    vault_save_config(VKEY_PERSONA, &g_persona, sizeof(g_persona));
    vault_save_config(VKEY_CONTROLS, &g_controls_side, sizeof(g_controls_side));
    vault_save_config(VKEY_SCHEME, &g_color_scheme, sizeof(g_color_scheme));
    vault_save_config(VKEY_WALLPAPER, &g_wallpaper, sizeof(g_wallpaper));
    vault_save_config(VKEY_MOUSE_SPEED, &g_mouse_speed, sizeof(g_mouse_speed));
    vault_save_config(VKEY_KEY_REPEAT, &g_key_repeat, sizeof(g_key_repeat));
    /* J.2: access config is owned + persisted by access.c (access_set_* ->
     * access_save); the settings GUI no longer keeps or saves a private copy. */

    /* E.4: flash the cursor confirm checkmark on a committed settings change. */
    { extern void cursor_confirm(void); cursor_confirm(); }
}

static void load_all(void)
{
    vault_load_config(VKEY_PERSONA, &g_persona, sizeof(g_persona));
    vault_load_config(VKEY_CONTROLS, &g_controls_side, sizeof(g_controls_side));
    vault_load_config(VKEY_SCHEME, &g_color_scheme, sizeof(g_color_scheme));
    vault_load_config(VKEY_WALLPAPER, &g_wallpaper, sizeof(g_wallpaper));
    vault_load_config(VKEY_MOUSE_SPEED, &g_mouse_speed, sizeof(g_mouse_speed));
    vault_load_config(VKEY_KEY_REPEAT, &g_key_repeat, sizeof(g_key_repeat));
    /* J.2: access config loaded by access_init() from its own VAULT key. */
}

/* ── Apply changes to live subsystems ── */

static void apply_persona(void)
{
    /* Persona is applied through shell commands internally.
     * Settings stores the value; the shell reads it on next prompt. */
    vault_save_config(VKEY_PERSONA, &g_persona, sizeof(g_persona));
}

static void apply_controls(void)
{
    wm_set_controls_side(g_controls_side == 0 ? WM_CONTROLS_LEFT : WM_CONTROLS_RIGHT);
}

/* ── Page drawing ── */

static void draw_page_display(int x, int y, int w, int h)
{
    (void)h;
    int sel = g_settings.selected_item;

    /* Row 0: Persona */
    static const char *personas[] = { "Zeros", "DereZ", "Zeos" };
    draw_row_radio(x, y, w, 0, "Persona", personas, 3, g_persona, sel == 0);

    /* Row 1: Window Controls */
    static const char *sides[] = { "Left", "Right" };
    draw_row_radio(x, y, w, 1, "Window Controls", sides, 2, g_controls_side, sel == 1);

    /* Row 2: Color Scheme */
    static const char *schemes[] = { "Dark", "Light", "Auto" };
    draw_row_radio(x, y, w, 2, "Color Scheme", schemes, 3, (int)g_color_scheme, sel == 2);

    /* Row 3: Wallpaper Color */
    char hexbuf[10];
    hex_to_str(g_wallpaper & 0x00FFFFFF, hexbuf, sizeof(hexbuf));
    draw_row(x, y, w, 3, "Wallpaper Color", hexbuf, sel == 3);
}

static void draw_page_input(int x, int y, int w, int h)
{
    (void)h;
    int sel = g_settings.selected_item;
    char buf[16];

    /* Row 0: Mouse Speed */
    char *ms = int_to_str(g_mouse_speed, buf, sizeof(buf));
    draw_row(x, y, w, 0, "Mouse Speed", ms, sel == 0);

    /* Row 1: Key Repeat Rate */
    char *kr = int_to_str(g_key_repeat, buf, sizeof(buf));
    draw_row(x, y, w, 1, "Key Repeat (ms)", kr, sel == 1);
}

static void draw_page_accessibility(int x, int y, int w, int h)
{
    (void)h;
    int sel = g_settings.selected_item;
    const access_config_t *acc = access_get();   /* J.2: the REAL live config */

    /* Row 0: Sensory Mode */
    static const char *sensory[] = { "Standard", "Low Stimuli", "High Contrast" };
    draw_row_radio(x, y, w, 0, "Sensory Mode", sensory, 3, (int)acc->sensory, sel == 0);

    /* Row 1: Density */
    static const char *density[] = { "Comfortable", "Standard", "Compact" };
    draw_row_radio(x, y, w, 1, "Density", density, 3, (int)acc->density, sel == 1);

    /* Row 2: Reduced Motion */
    static const char *onoff[] = { "Off", "On" };
    draw_row_radio(x, y, w, 2, "Reduced Motion", onoff, 2, acc->reduced_motion, sel == 2);

    /* Row 3: Animation Speed (access stores a float; map to the 4 UI steps) */
    static const char *speeds[] = { "0x", "0.5x", "1x", "2x" };
    draw_row_radio(x, y, w, 3, "Animation Speed", speeds, 4, anim_speed_to_idx(acc->anim_speed), sel == 3);

    /* Row 4: Letter Spacing */
    static const char *spacing[] = { "0", "1", "2", "3", "4" };
    draw_row_radio(x, y, w, 4, "Letter Spacing", spacing, 5, acc->letter_spacing, sel == 4);

    /* Row 5: Focus Mode */
    draw_row_radio(x, y, w, 5, "Focus Mode", onoff, 2, acc->focus_mode, sel == 5);

    /* Row 6: Night Shift */
    draw_row_radio(x, y, w, 6, "Night Shift", onoff, 2, acc->night_shift, sel == 6);
}

static void draw_page_network(int x, int y, int w, int h)
{
    (void)h;
    char buf[20];

    ip_to_str(g_net.ip, buf, sizeof(buf));
    draw_row(x, y, w, 0, "IP Address", buf, 0);

    ip_to_str(g_net.gateway, buf, sizeof(buf));
    draw_row(x, y, w, 1, "Gateway", buf, 0);

    ip_to_str(g_net.dns, buf, sizeof(buf));
    draw_row(x, y, w, 2, "DNS", buf, 0);

    const struct dhcp_lease *lease = dhcp_get_lease();
    if (lease && lease->valid) {
        char lbuf[16];
        char *ls = int_to_str((int)lease->lease_time, lbuf, sizeof(lbuf));
        /* Append "s" for seconds */
        int len = 0;
        while (ls[len]) len++;
        if (len < (int)sizeof(lbuf) - 1) { ls[len] = 's'; ls[len + 1] = '\0'; }
        draw_row(x, y, w, 3, "DHCP Lease", ls, 0);
    } else {
        draw_row(x, y, w, 3, "DHCP Lease", "N/A", 0);
    }
}

static void draw_page_about(int x, int y, int w, int h)
{
    (void)h;
    (void)w;
    char buf[32];
    uint32_t label_color = (COLOR_ON_SURFACE & 0x00FFFFFF) | ((uint32_t)TEXT_SECONDARY << 24);
    uint32_t value_color = (COLOR_ON_SURFACE & 0x00FFFFFF) | ((uint32_t)TEXT_PRIMARY << 24);

    /* Title */
    font_draw(x, y + 8, "Z-OS Alpha 0.1", FONT_UI_BOLD, TYPE_HEADING, theme_accent());

    /* Tagline */
    font_draw(x, y + 40, "Chains that produce an OS as a side effect.",
              FONT_UI_LIGHT, TYPE_BODY, label_color);

    /* Chain count */
    int row_y = y + 80;
    font_draw(x, row_y, "Chains:", FONT_UI, TYPE_BODY, label_color);
    char *cc = int_to_str(chain_count(), buf, sizeof(buf));
    font_draw(x + 120, row_y, cc, FONT_UI, TYPE_BODY, value_color);

    /* Uptime */
    row_y += ROW_HEIGHT;
    uint64_t tsc_now = timer_read_tsc();
    uint64_t freq = timer_tsc_freq();
    uint64_t uptime_s = freq ? tsc_now / freq : 0;
    uint64_t mins = uptime_s / 60;
    uint64_t secs = uptime_s % 60;

    char upbuf[32];
    char *m = int_to_str((int)mins, buf, sizeof(buf));
    int pos = 0;
    while (*m && pos < 28) upbuf[pos++] = *m++;
    upbuf[pos++] = 'm';
    upbuf[pos++] = ' ';
    char *s = int_to_str((int)secs, buf, sizeof(buf));
    while (*s && pos < 30) upbuf[pos++] = *s++;
    upbuf[pos++] = 's';
    upbuf[pos] = '\0';

    font_draw(x, row_y, "Uptime:", FONT_UI, TYPE_BODY, label_color);
    font_draw(x + 120, row_y, upbuf, FONT_UI, TYPE_BODY, value_color);

    /* Build date */
    row_y += ROW_HEIGHT;
    font_draw(x, row_y, "Build:", FONT_UI, TYPE_BODY, label_color);
    font_draw(x + 120, row_y, __DATE__ " " __TIME__, FONT_UI, TYPE_BODY, value_color);
}

/* ── Sidebar ── */

static void draw_sidebar(int x, int y, int h)
{
    /* Sidebar background */
    fb_rect(x, y, SIDEBAR_W, h, COLOR_SURFACE_HIGH);

    /* Separator line */
    fb_vline(x + SIDEBAR_W - 1, y, h, COLOR_SEPARATOR);

    /* Page entries */
    for (int i = 0; i < SETTINGS_PAGE_COUNT; i++) {
        int ry = y + Z6 + i * ROW_HEIGHT;

        if (i == (int)g_settings.page) {
            /* Selected page: accent background strip */
            fb_rect(x, ry, SIDEBAR_W - 1, ROW_HEIGHT, theme_accent_dim());
            /* Left accent bar */
            fb_rect(x, ry, 3, ROW_HEIGHT, theme_accent());
            font_draw(x + Z4, ry + 8, page_names[i], FONT_UI, TYPE_BODY, theme_accent());
        } else {
            font_draw(x + Z4, ry + 8, page_names[i], FONT_UI, TYPE_BODY, COLOR_ON_SURFACE_2);
        }
    }
}

/* ── Main draw callback (called by WM) ── */

void settings_draw(int id, int x, int y, int w, int h)
{
    (void)id;

    /* Background */
    fb_rect(x, y, w, h, COLOR_SURFACE);

    /* Sidebar */
    draw_sidebar(x, y, h);

    /* Content area */
    int cx = x + SIDEBAR_W + PAGE_PADDING;
    int cy = y + PAGE_PADDING - g_settings.scroll_y;
    int cw = w - SIDEBAR_W - PAGE_PADDING * 2;

    switch (g_settings.page) {
    case SETTINGS_PAGE_DISPLAY:
        draw_page_display(cx, cy, cw, h);
        break;
    case SETTINGS_PAGE_INPUT:
        draw_page_input(cx, cy, cw, h);
        break;
    case SETTINGS_PAGE_ACCESSIBILITY:
        draw_page_accessibility(cx, cy, cw, h);
        break;
    case SETTINGS_PAGE_NETWORK:
        draw_page_network(cx, cy, cw, h);
        break;
    case SETTINGS_PAGE_ABOUT:
        draw_page_about(cx, cy, cw, h);
        break;
    default:
        break;
    }
}

/* ── Keyboard input ── */

/* Scancodes (PS/2 set 1) */
#define SC_UP      0x48
#define SC_DOWN    0x50
#define SC_LEFT    0x4B
#define SC_RIGHT   0x4D
#define SC_ENTER   0x1C
#define SC_TAB     0x0F
#define SC_ESC     0x01

/*
 * Cycle a setting value forward for the current page + item.
 * direction: +1 = forward, -1 = backward.
 */
static void cycle_value(int direction)
{
    int item = g_settings.selected_item;

    switch (g_settings.page) {
    case SETTINGS_PAGE_DISPLAY:
        switch (item) {
        case 0: /* Persona */
            g_persona = (g_persona + direction + 3) % 3;
            apply_persona();
            break;
        case 1: /* Controls side */
            g_controls_side = (g_controls_side + direction + 2) % 2;
            apply_controls();
            break;
        case 2: /* Color scheme */
            g_color_scheme = (color_scheme_t)((g_color_scheme + direction + 3) % 3);
            break;
        case 3: /* Wallpaper — no cycle, placeholder */
            break;
        }
        break;

    case SETTINGS_PAGE_INPUT:
        switch (item) {
        case 0: /* Mouse speed */
            g_mouse_speed = clamp(g_mouse_speed + direction, 1, 10);
            break;
        case 1: /* Key repeat */
            g_key_repeat = clamp(g_key_repeat + direction * 5, 10, 200);
            break;
        }
        break;

    case SETTINGS_PAGE_ACCESSIBILITY:
        switch (item) {
        /* J.2: route through access_set_* so changes apply live to the M.4
         * consumers and persist via access.c -- no private copy. */
        case 0: /* Sensory */
            access_set_sensory((sensory_mode_t)((access_get()->sensory + direction + 3) % 3));
            break;
        case 1: /* Density */
            access_set_density((density_mode_t)((access_get()->density + direction + 3) % 3));
            break;
        case 2: /* Reduced motion */
            access_set_reduced_motion(!access_get()->reduced_motion);
            break;
        case 3: /* Anim speed (4 UI steps -> float) */
            access_set_anim_speed(
                ANIM_SPEED_STEPS[(anim_speed_to_idx(access_get()->anim_speed) + direction + 4) % 4]);
            break;
        case 4: /* Letter spacing (no dedicated setter -> write + persist) */
            access_get()->letter_spacing = clamp(access_get()->letter_spacing + direction, 0, 4);
            access_save();
            break;
        case 5: /* Focus mode */
            access_set_focus_mode(!access_get()->focus_mode);
            break;
        case 6: /* Night shift (no dedicated setter -> write + persist + redraw) */
            access_get()->night_shift = !access_get()->night_shift;
            access_save();
            { extern void compositor_dirty_all(void); compositor_dirty_all(); }
            break;
        }
        break;

    case SETTINGS_PAGE_NETWORK:
    case SETTINGS_PAGE_ABOUT:
        /* Read-only pages */
        break;

    default:
        break;
    }

    save_all();
}

void settings_key(int scancode)
{
    if (!g_open) return;

    int max_items = items_per_page[g_settings.page];

    switch (scancode) {
    case SC_UP:
        if (g_settings.selected_item > 0)
            g_settings.selected_item--;
        break;

    case SC_DOWN:
        if (g_settings.selected_item < max_items - 1)
            g_settings.selected_item++;
        break;

    case SC_LEFT:
        cycle_value(-1);
        break;

    case SC_RIGHT:
    case SC_ENTER:
        cycle_value(+1);
        break;

    case SC_TAB:
        /* Tab cycles pages */
        g_settings.page = (settings_page_t)((g_settings.page + 1) % SETTINGS_PAGE_COUNT);
        g_settings.selected_item = 0;
        g_settings.scroll_y = 0;
        break;

    case SC_ESC:
        settings_close();
        break;
    }
}

/* ── Mouse click ── */

void settings_click(int x, int y)
{
    if (!g_open) return;

    chain_surface_t *surf = wm_get_surface(g_settings.surface_id);
    if (!surf) return;

    /* Convert to local coordinates */
    int lx = x - surf->x;
    int ly = y - surf->y - WM_TITLEBAR_HEIGHT;

    if (lx < 0 || ly < 0) return;

    /* Sidebar click — select page */
    if (lx < SIDEBAR_W) {
        int idx = (ly - Z6) / ROW_HEIGHT;
        if (idx >= 0 && idx < SETTINGS_PAGE_COUNT) {
            g_settings.page = (settings_page_t)idx;
            g_settings.selected_item = 0;
            g_settings.scroll_y = 0;
        }
        return;
    }

    /* Content area click — select item */
    int content_y = ly - PAGE_PADDING + g_settings.scroll_y;
    int idx = content_y / ROW_HEIGHT;
    int max_items = items_per_page[g_settings.page];
    if (idx >= 0 && idx < max_items) {
        g_settings.selected_item = idx;
        /* Click also cycles the value forward */
        cycle_value(+1);
    }
}

/* ── Open / Close ── */

void settings_open(void)
{
    if (g_open) {
        /* Already open — just focus */
        wm_focus_surface(g_settings.surface_id);
        return;
    }

    /* Load persisted settings */
    load_all();

    /* Create a WM surface */
    int sw = 640;
    int sh = 420;
    int sx = ((int)fb_width() - sw) / 2;
    int sy = ((int)fb_height() - sh) / 2;

    g_settings.surface_id = wm_create_surface("Settings", -1,
                                               sx, sy, sw, sh,
                                               settings_draw);
    if (g_settings.surface_id < 0) return;

    g_settings.page = SETTINGS_PAGE_DISPLAY;
    g_settings.selected_item = 0;
    g_settings.scroll_y = 0;
    g_open = 1;

    wm_focus_surface(g_settings.surface_id);
}

/* J.4: open Settings jumped to a specific page ("Settings for this <element>"
 * routes here with the element's relevant page). */
void settings_open_page(int page)
{
    if (page < 0 || page >= SETTINGS_PAGE_COUNT) page = SETTINGS_PAGE_DISPLAY;
    settings_open();
    g_settings.page = page;
    g_settings.selected_item = 0;
    g_settings.scroll_y = 0;
    { extern void compositor_dirty_all(void); compositor_dirty_all(); }
}

void settings_close(void)
{
    if (!g_open) return;

    save_all();
    wm_detach_surface(g_settings.surface_id);
    g_settings.surface_id = -1;
    g_open = 0;
}

/* ── Query ── */

const access_config_t *settings_get_access(void)
{
    /* J.2: single source of truth -- the live access subsystem config. */
    return access_get();
}

#include "kprint.h"
/* J.2 selftest: prove the settings GUI now mutates the ONE real access config
 * (access_get()) rather than a private duplicate. Drive the accessibility page's
 * cycle_value and confirm the live subsystem reflects each change. Also confirm
 * settings_get_access() IS access_get() (same pointer = same state).
 * Un-gated so the production `selftest` shell can run it; PRODUCTION-SAFE: the
 * cycle_value calls route through access_set_sensory/reduced_motion/focus_mode
 * (which persist to VAULT), so this saves the originals (page, selected_item, and
 * all 3 access fields) up front and restores them via the same setters at the end
 * -- net no change (the FINAL setter writes return each field to its boot value).
 * s0/r0/f0 are the true originals (each field is only touched by its own cycle).
 * color_temp rides along: access_set_sensory recomputes it from the mode, so
 * restoring sensory=STANDARD resets color_temp=0 (valid while night_shift=0, the
 * boot default; if night_shift ever ships enabled, save/restore color_temp too).
 * VAULT persists 6x (3 mutate + 3 restore), net-restored -- accepted G.4/M.7
 * pattern. Returns 1 on PASS. */
int settings_j2_selftest(void)
{
    /* snapshot everything this selftest mutates */
    int          saved_page = g_settings.page;
    int          saved_item = g_settings.selected_item;

    int shared_ptr = (settings_get_access() == access_get());

    g_settings.page = SETTINGS_PAGE_ACCESSIBILITY;

    int s0 = (int)access_get()->sensory;
    g_settings.selected_item = 0; cycle_value(1);
    int sensory_live = ((int)access_get()->sensory == (s0 + 1) % 3);

    int r0 = access_get()->reduced_motion;
    g_settings.selected_item = 2; cycle_value(1);
    int rm_live = (access_get()->reduced_motion == !r0);

    int f0 = access_get()->focus_mode;
    g_settings.selected_item = 5; cycle_value(1);
    int focus_live = (access_get()->focus_mode == !f0);

    /* restore the exact pre-test state via the real setters (net-restored VAULT) */
    access_set_sensory((sensory_mode_t)s0);
    access_set_reduced_motion(r0);
    access_set_focus_mode(f0);
    g_settings.page = saved_page;
    g_settings.selected_item = saved_item;

    int pass = shared_ptr && sensory_live && rm_live && focus_live;
    kputs("[J2] shared_ptr="); kput_dec((uint64_t)shared_ptr);
    kputs(" sensory_live="); kput_dec((uint64_t)sensory_live);
    kputs(" reduced_motion_live="); kput_dec((uint64_t)rm_live);
    kputs(" focus_live="); kput_dec((uint64_t)focus_live);
    kputs(" s0="); kput_dec((uint64_t)s0);
    kputs(" r0="); kput_dec((uint64_t)r0);
    kputs(" f0="); kput_dec((uint64_t)f0);
    kputs(pass ? " -> PASS\n" : " -> FAIL\n");
    return pass;
}

#ifdef ZEOS_DIAG_J1
/* J.1 selftest: the Settings app persists to VAULT and reloads correctly.
 * Set known values -> save_all -> clobber in-memory -> load_all -> assert the
 * VAULT round-trip restored them (an independent read across the VAULT boundary,
 * G3). Restores the originals afterward. */
void settings_j1_selftest(void)
{
    int o_ms = g_mouse_speed, o_kr = g_key_repeat, o_wp = g_wallpaper;

    g_mouse_speed = 7; g_key_repeat = 3; g_wallpaper = 2;
    save_all();
    g_mouse_speed = 99; g_key_repeat = 99; g_wallpaper = 99;   /* clobber */
    load_all();
    int ms_ok = (g_mouse_speed == 7);
    int kr_ok = (g_key_repeat == 3);
    int wp_ok = (g_wallpaper == 2);

    /* restore */
    g_mouse_speed = o_ms; g_key_repeat = o_kr; g_wallpaper = o_wp; save_all();

    int pass = ms_ok && kr_ok && wp_ok;
    kputs("[J1] vault round-trip mouse_speed="); kput_dec((uint64_t)ms_ok);
    kputs(" key_repeat="); kput_dec((uint64_t)kr_ok);
    kputs(" wallpaper="); kput_dec((uint64_t)wp_ok);
    kputs(pass ? " -> PASS\n" : " -> FAIL\n");
}
#endif

int settings_current_page(void) { return g_settings.page; }
int settings_is_open(void) { return g_open; }

#ifdef ZEOS_DIAG_J4
void settings_j4_selftest(void)
{
    /* "Settings for this…" opens Settings jumped to the element's page. */
    settings_close();
    settings_open_page(SETTINGS_PAGE_INPUT);
    int open_input = settings_is_open() && (settings_current_page() == SETTINGS_PAGE_INPUT);
    settings_open_page(SETTINGS_PAGE_ACCESSIBILITY);   /* re-route while open */
    int reroute = (settings_current_page() == SETTINGS_PAGE_ACCESSIBILITY);
    settings_close();
    int pass = open_input && reroute;
    kputs("[J4] open_on_input="); kput_dec((uint64_t)open_input);
    kputs(" reroute_to_a11y="); kput_dec((uint64_t)reroute);
    kputs(pass ? " -> PASS\n" : " -> FAIL\n");
}
#endif
