/*
 * Zeos -- First Boot Wizard
 *
 * Five screens, each teaching one Z-OS concept through interaction.
 * Uses the 8x16 boot font (no TTF dependency), framebuffer primitives,
 * and raw PS/2 keyboard input for arrow key support.
 *
 * Alpha 0.1 -- functional, not pretty. Spring animations are simple
 * frame-stepped pulses until the animation system is wired into a
 * proper frame loop.
 */

#include "firstboot.h"
#include "fb.h"
#include "theme.h"
#include "vault.h"   /* persist "system/firstboot_complete" (N.3) */
#include "keyboard.h"
#include "timer.h"
#include "io.h"
#include "kprint.h"

/* ── State ── */

static int fb_completed = 0;  /* Static flag until VAULT is wired */

/* ── Raw scancode reading for arrow keys ── */

/*
 * PS/2 scan code set 1 arrow keys (no E0 prefix handling needed
 * for basic numpad-style arrows on most keyboards, but real arrows
 * send E0 prefix). We read raw scancodes from port 0x60.
 */
#define SC_UP       0x48
#define SC_DOWN     0x50
#define SC_LEFT     0x4B
#define SC_RIGHT    0x4D
#define SC_ENTER    0x1C
#define SC_SPACE    0x39
#define SC_TAB      0x0F
#define SC_E0       0xE0   /* Extended key prefix */

/* Key codes returned by fb_getkey() */
#define KEY_NONE    0
#define KEY_LEFT    1
#define KEY_RIGHT   2
#define KEY_UP      3
#define KEY_DOWN    4
#define KEY_ENTER   5
#define KEY_SPACE   6
#define KEY_TAB     7

/*
 * Wait for a key press and return a simplified key code.
 * Handles E0-prefixed arrow keys and basic ASCII keys.
 * Blocks until a recognized key is pressed.
 */
static int fb_getkey(void)
{
    for (;;) {
        /* Wait for keyboard data */
        while (!(inb(0x64) & 0x01))
            __asm__ volatile("hlt");

        uint8_t sc = inb(0x60);

        /* Skip key releases (bit 7 set), except E0 */
        if (sc == SC_E0) {
            /* Next scancode is the actual key */
            while (!(inb(0x64) & 0x01))
                __asm__ volatile("hlt");
            sc = inb(0x60);
            /* Skip release of extended key */
            if (sc & 0x80)
                continue;
            switch (sc) {
            case SC_UP:    return KEY_UP;
            case SC_DOWN:  return KEY_DOWN;
            case SC_LEFT:  return KEY_LEFT;
            case SC_RIGHT: return KEY_RIGHT;
            default:       continue;
            }
        }

        if (sc & 0x80)
            continue;  /* Release event */

        switch (sc) {
        case SC_UP:    return KEY_UP;
        case SC_DOWN:  return KEY_DOWN;
        case SC_LEFT:  return KEY_LEFT;
        case SC_RIGHT: return KEY_RIGHT;
        case SC_ENTER: return KEY_ENTER;
        case SC_SPACE: return KEY_SPACE;
        case SC_TAB:   return KEY_TAB;
        default:       continue;
        }
    }
}

/* ── Drawing helpers ── */

/* Width of a string in pixels (8px per char, boot font) */
static int text_width(const char *s)
{
    int w = 0;
    while (*s) {
        if (*s != '\n') w++;
        s++;
    }
    return w * 8;
}

/* Draw text centered horizontally at a given y */
static void draw_centered(int y, const char *s, uint32_t color)
{
    int w = text_width(s);
    int x = ((int)fb_width() - w) / 2;
    if (x < 0) x = 0;
    fb_text(x, y, s, color);
}

/* Draw a filled rounded-ish card (rect with accent border) */
static void draw_card(int x, int y, int w, int h,
                      uint32_t fill, uint32_t border, int selected)
{
    /* Fill */
    fb_rect(x, y, w, h, fill);
    /* Border */
    int thick = selected ? 3 : 1;
    fb_rect_outline(x, y, w, h, border, thick);
}

/* Simple delay using timer_wait_ms or busy loop */
static void delay_ms(uint32_t ms)
{
    timer_wait_ms(ms);
}

/* Dim a color by blending toward black (rough: shift RGB right by 1) */
static uint32_t color_dim(uint32_t c)
{
    uint32_t a = c & 0xFF000000;
    uint32_t r = ((c >> 16) & 0xFF) >> 1;
    uint32_t g = ((c >> 8) & 0xFF) >> 1;
    uint32_t b = (c & 0xFF) >> 1;
    return a | (r << 16) | (g << 8) | b;
}

/* ── Persona colors lookup ── */

static uint32_t persona_accent(int p)
{
    switch (p) {
    case 0: return COLOR_ZEROS_ACCENT;
    case 1: return COLOR_DEREZ_ACCENT;
    case 2: return COLOR_FULL_ACCENT;
    }
    return COLOR_FULL_ACCENT;
}

/* ── Screen 1: Welcome ── */

static void screen_welcome(void)
{
    uint32_t sw = fb_width();
    uint32_t sh = fb_height();
    int cx = (int)sw / 2;
    int cy = (int)sh / 2;

    fb_clear(COLOR_SURFACE);

    /* "Z-OS" centered, large -- draw at TYPE_DISPLAY scale using boot font.
     * Boot font is 8x16, so we draw it at 1x and rely on visual weight
     * from the centered layout. */
    draw_centered(cy - 80, "Z - O S", COLOR_ON_SURFACE);

    /* Subtitle */
    draw_centered(cy - 40, "Z-OS runs on signals -- everything is connected.",
                  COLOR_ON_SURFACE_2);

    /* Three circles connected by lines -- signal visualization */
    int dot_r  = 8;
    int spread = 80;
    int dot_y  = cy + 20;
    int x0 = cx - spread;
    int x1 = cx;
    int x2 = cx + spread;

    /* Lines between dots */
    fb_line(x0 + dot_r, dot_y, x1 - dot_r, dot_y, COLOR_ON_SURFACE_3);
    fb_line(x1 + dot_r, dot_y, x2 - dot_r, dot_y, COLOR_ON_SURFACE_3);

    /* Draw three dots */
    fb_circle_filled(x0, dot_y, dot_r, COLOR_FULL_ACCENT);
    fb_circle_filled(x1, dot_y, dot_r, COLOR_FULL_ACCENT);
    fb_circle_filled(x2, dot_y, dot_r, COLOR_FULL_ACCENT);

    /* Pulse animation: briefly brighten each dot in sequence */
    for (int pulse = 0; pulse < 2; pulse++) {
        int dots[3] = { x0, x1, x2 };
        for (int d = 0; d < 3; d++) {
            fb_circle_filled(dots[d], dot_y, dot_r + 2, COLOR_ON_SURFACE);
            delay_ms(120);
            fb_circle_filled(dots[d], dot_y, dot_r + 2, COLOR_SURFACE);
            fb_circle_filled(dots[d], dot_y, dot_r, COLOR_FULL_ACCENT);
            delay_ms(60);
        }
    }

    /* Prompt */
    draw_centered(cy + 80, "Press Enter or Space to continue",
                  COLOR_ON_SURFACE_3);

    /* Wait for Enter or Space */
    for (;;) {
        int key = fb_getkey();
        if (key == KEY_ENTER || key == KEY_SPACE)
            break;
    }
}

/* ── Screen 2: Persona ── */

static int screen_persona(void)
{
    uint32_t sw = fb_width();
    uint32_t sh = fb_height();

    int card_w = 160;
    int card_h = 100;
    int gap    = 24;
    int total  = card_w * 3 + gap * 2;
    int base_x = ((int)sw - total) / 2;
    int card_y = (int)sh / 2 - card_h / 2 - 20;

    int sel = 2;  /* Default: Full */

    const char *names[3]  = { "Zeros", "DereZ", "Full" };
    const char *descs[3]  = { "Build, make, explore",
                              "Code, create, ship",
                              "Everything" };
    uint32_t accents[3]   = { COLOR_ZEROS_ACCENT,
                              COLOR_DEREZ_ACCENT,
                              COLOR_FULL_ACCENT };

    for (;;) {
        fb_clear(COLOR_SURFACE);

        /* Title */
        draw_centered(card_y - 60, "Choose your persona", COLOR_ON_SURFACE);

        /* Three cards */
        for (int i = 0; i < 3; i++) {
            int cx = base_x + i * (card_w + gap);
            int is_sel = (i == sel);

            /* Card background: dim tint of accent for selected, surface for others */
            uint32_t fill = is_sel ? color_dim(color_dim(accents[i]))
                                   : COLOR_SURFACE_HIGH;
            uint32_t border = is_sel ? accents[i] : COLOR_ON_SURFACE_3;

            draw_card(cx, card_y, card_w, card_h, fill, border, is_sel);

            /* Name centered in card */
            int name_x = cx + (card_w - text_width(names[i])) / 2;
            fb_text(name_x, card_y + 20, names[i],
                    is_sel ? COLOR_ON_SURFACE : COLOR_ON_SURFACE_2);

            /* Description centered in card */
            int desc_x = cx + (card_w - text_width(descs[i])) / 2;
            fb_text(desc_x, card_y + 50, descs[i],
                    is_sel ? COLOR_ON_SURFACE_2 : COLOR_ON_SURFACE_3);
        }

        /* Explanation */
        draw_centered(card_y + card_h + 30,
            "Your persona shapes what you see first.",
            COLOR_ON_SURFACE_2);
        draw_centered(card_y + card_h + 50,
            "The full system is always underneath.",
            COLOR_ON_SURFACE_3);

        /* Navigation hint */
        draw_centered(card_y + card_h + 90,
            "Arrow keys to choose, Enter to confirm",
            COLOR_ON_SURFACE_3);

        int key = fb_getkey();
        if (key == KEY_LEFT && sel > 0)  sel--;
        if (key == KEY_RIGHT && sel < 2) sel++;
        if (key == KEY_ENTER)            return sel;
    }
}

/* ── Screen 3: Window Controls ── */

static int screen_controls(void)
{
    uint32_t sw = fb_width();
    uint32_t sh = fb_height();
    int cx = (int)sw / 2;

    /* Mock window dimensions */
    int win_w = 400;
    int win_h = 200;
    int win_x = cx - win_w / 2;
    int win_y = (int)sh / 2 - win_h / 2 - 40;
    int titlebar_h = 32;

    int sel = 0;  /* 0 = Left, 1 = Right */

    const char *btn_labels[4] = { "x", "-", "=", "*" };

    for (;;) {
        fb_clear(COLOR_SURFACE);

        draw_centered(win_y - 50, "Where do you want window controls?",
                      COLOR_ON_SURFACE);

        /* Mock window */
        fb_rect(win_x, win_y, win_w, win_h, COLOR_SURFACE_HIGH);
        fb_rect_outline(win_x, win_y, win_w, win_h, COLOR_ON_SURFACE_3, 1);

        /* Title bar */
        fb_rect(win_x, win_y, win_w, titlebar_h, COLOR_SURFACE_TOP);

        /* Title text */
        fb_text(win_x + win_w / 2 - 32, win_y + 8, "My Chain",
                COLOR_ON_SURFACE_2);

        /* Window control buttons on selected side */
        int btn_size = 16;
        int btn_pad  = 6;
        int btn_y    = win_y + (titlebar_h - btn_size) / 2;

        for (int i = 0; i < 4; i++) {
            int btn_x;
            if (sel == 0) {
                /* Left side */
                btn_x = win_x + 8 + i * (btn_size + btn_pad);
            } else {
                /* Right side */
                btn_x = win_x + win_w - 8 - (4 - i) * (btn_size + btn_pad);
            }
            fb_rect_outline(btn_x, btn_y, btn_size, btn_size,
                            COLOR_ON_SURFACE_3, 1);
            fb_text(btn_x + 4, btn_y, btn_labels[i], COLOR_ON_SURFACE_2);
        }

        /* Selection buttons */
        int opt_y = win_y + win_h + 30;
        int opt_w = 100;
        int opt_h = 36;
        int opt_gap = 40;

        /* Left button */
        int lx = cx - opt_w - opt_gap / 2;
        draw_card(lx, opt_y, opt_w, opt_h,
                  sel == 0 ? color_dim(COLOR_FULL_ACCENT) : COLOR_SURFACE_HIGH,
                  sel == 0 ? COLOR_FULL_ACCENT : COLOR_ON_SURFACE_3,
                  sel == 0);
        fb_text(lx + (opt_w - text_width("Left")) / 2, opt_y + 10, "Left",
                sel == 0 ? COLOR_ON_SURFACE : COLOR_ON_SURFACE_2);

        /* Right button */
        int rx = cx + opt_gap / 2;
        draw_card(rx, opt_y, opt_w, opt_h,
                  sel == 1 ? color_dim(COLOR_FULL_ACCENT) : COLOR_SURFACE_HIGH,
                  sel == 1 ? COLOR_FULL_ACCENT : COLOR_ON_SURFACE_3,
                  sel == 1);
        fb_text(rx + (opt_w - text_width("Right")) / 2, opt_y + 10, "Right",
                sel == 1 ? COLOR_ON_SURFACE : COLOR_ON_SURFACE_2);

        /* Explanation */
        draw_centered(opt_y + opt_h + 30,
            "In Z-OS, closing a window doesn't stop its work.",
            COLOR_ON_SURFACE_2);
        draw_centered(opt_y + opt_h + 50,
            "Your chains keep running.",
            COLOR_ON_SURFACE_3);

        /* Navigation */
        draw_centered(opt_y + opt_h + 80,
            "Arrow keys to pick, Enter to confirm",
            COLOR_ON_SURFACE_3);

        int key = fb_getkey();
        if (key == KEY_LEFT)  sel = 0;
        if (key == KEY_RIGHT) sel = 1;
        if (key == KEY_ENTER) return sel;
    }
}

/* ── Screen 4: Appearance ── */

static void screen_appearance(struct firstboot_config *cfg)
{
    uint32_t sw = fb_width();
    uint32_t sh = fb_height();

    int card_w = 130;
    int card_h = 60;
    int gap    = 20;

    /* Two rows: theme (top), density (bottom) */
    int row = 0;       /* 0 = theme row, 1 = density row */
    int theme_sel   = 1;  /* Default: Dark */
    int density_sel = 1;  /* Default: Standard */

    const char *theme_names[3]   = { "Light", "Dark", "Auto" };
    const char *density_names[3] = { "Comfortable", "Standard", "Compact" };

    uint32_t theme_fills[3] = {
        0xFFE0E4E8,   /* Light preview: light surface */
        COLOR_SURFACE, /* Dark preview: dark surface */
        COLOR_SURFACE_HIGH  /* Auto preview: mid */
    };

    for (;;) {
        fb_clear(COLOR_SURFACE);

        int total = card_w * 3 + gap * 2;
        int base_x = ((int)sw - total) / 2;

        draw_centered((int)sh / 2 - 160, "Appearance", COLOR_ON_SURFACE);

        /* Theme row */
        int theme_y = (int)sh / 2 - 100;
        draw_centered(theme_y - 24, "Theme", COLOR_ON_SURFACE_2);

        for (int i = 0; i < 3; i++) {
            int cx = base_x + i * (card_w + gap);
            int is_sel = (row == 0 && i == theme_sel);

            draw_card(cx, theme_y, card_w, card_h,
                      theme_fills[i],
                      is_sel ? COLOR_FULL_ACCENT : COLOR_ON_SURFACE_3,
                      is_sel);

            /* Text color depends on card fill */
            uint32_t tc = (i == 0) ? COLOR_SURFACE : COLOR_ON_SURFACE;
            int name_x = cx + (card_w - text_width(theme_names[i])) / 2;
            fb_text(name_x, theme_y + 22, theme_names[i], tc);
        }

        /* Density row */
        int density_y = theme_y + card_h + 50;
        draw_centered(density_y - 24, "Density", COLOR_ON_SURFACE_2);

        for (int i = 0; i < 3; i++) {
            int cx = base_x + i * (card_w + gap);
            int is_sel = (row == 1 && i == density_sel);

            draw_card(cx, density_y, card_w, card_h,
                      COLOR_SURFACE_HIGH,
                      is_sel ? COLOR_FULL_ACCENT : COLOR_ON_SURFACE_3,
                      is_sel);

            int name_x = cx + (card_w - text_width(density_names[i])) / 2;
            fb_text(name_x, density_y + 22, density_names[i],
                    is_sel ? COLOR_ON_SURFACE : COLOR_ON_SURFACE_2);
        }

        /* Explanation */
        draw_centered(density_y + card_h + 40,
            "These aren't locked in.",
            COLOR_ON_SURFACE_2);
        draw_centered(density_y + card_h + 60,
            "Right-click anything in Z-OS to find its settings.",
            COLOR_ON_SURFACE_3);

        /* Navigation */
        draw_centered(density_y + card_h + 100,
            "Arrow keys to choose, Enter to confirm",
            COLOR_ON_SURFACE_3);

        int key = fb_getkey();
        int *sel = (row == 0) ? &theme_sel : &density_sel;

        if (key == KEY_LEFT && *sel > 0)   (*sel)--;
        if (key == KEY_RIGHT && *sel < 2)  (*sel)++;
        if (key == KEY_UP)                 row = 0;
        if (key == KEY_DOWN || key == KEY_TAB) row = 1;

        if (key == KEY_ENTER) {
            if (row == 0) {
                /* Move to density row */
                row = 1;
            } else {
                /* Done */
                cfg->theme   = (uint8_t)theme_sel;
                cfg->density = (uint8_t)density_sel;
                return;
            }
        }
    }
}

/* ── Screen 5: Done ── */

static void screen_done(void)
{
    uint32_t sw = fb_width();
    uint32_t sh = fb_height();
    int cx = (int)sw / 2;
    int cy = (int)sh / 2;

    fb_clear(COLOR_SURFACE);

    draw_centered(cy - 80, "Two ways to do anything:", COLOR_ON_SURFACE);

    /* Left: Command Palette */
    int left_x = cx - 240;
    fb_text(left_x, cy - 30, "Command Palette", COLOR_ON_SURFACE);
    fb_text(left_x, cy - 10, "Search for any action", COLOR_ON_SURFACE_2);
    fb_text(left_x, cy + 10, "(top-left corner)", COLOR_ON_SURFACE_3);

    /* Right: Dock */
    int right_x = cx + 40;
    fb_text(right_x, cy - 30, "Dock", COLOR_ON_SURFACE);
    fb_text(right_x, cy - 10, "Pin what you use", COLOR_ON_SURFACE_2);
    fb_text(right_x, cy + 10, "(bottom edge)", COLOR_ON_SURFACE_3);

    /* Divider */
    fb_vline(cx, cy - 40, 80, COLOR_ON_SURFACE_3);

    /* Glow animation: pulse top-left corner and bottom edge */
    int glow_size = 40;
    uint32_t glow_color = COLOR_FULL_ACCENT;

    for (int pulse = 0; pulse < 3; pulse++) {
        /* Top-left corner glow */
        fb_rect_blend(0, 0, glow_size, glow_size,
                      (0x30000000 | (glow_color & 0x00FFFFFF)));
        /* Bottom edge glow */
        fb_rect_blend(0, (int)sh - 4, (int)sw, 4,
                      (0x40000000 | (glow_color & 0x00FFFFFF)));
        delay_ms(200);

        /* Fade out */
        fb_rect(0, 0, glow_size, glow_size, COLOR_SURFACE);
        fb_rect(0, (int)sh - 4, (int)sw, 4, COLOR_SURFACE);
        delay_ms(100);
    }

    /* Final message */
    draw_centered(cy + 70, "Your space is ready.", COLOR_ON_SURFACE);
    draw_centered(cy + 110, "Press Enter to begin",
                  COLOR_ON_SURFACE_3);

    for (;;) {
        int key = fb_getkey();
        if (key == KEY_ENTER || key == KEY_SPACE)
            break;
    }
}

/* ── Public API ── */

int firstboot_should_run(void)
{
    if (fb_completed) return 0;
    /* Persisted flag survives reboots so first-run only shows once. */
    uint8_t done = 0;
    if (vault_load_config("system/firstboot_complete", &done, (uint32_t)sizeof(done))
            == (int)sizeof(done) && done)
        return 0;
    return 1;
}

void firstboot_mark_complete(void)
{
    fb_completed = 1;
    uint8_t done = 1;
    vault_save_config("system/firstboot_complete", &done, (uint32_t)sizeof(done));
}

struct firstboot_config firstboot_run(void)
{
    struct firstboot_config cfg;
    cfg.persona       = 2;  /* Full */
    cfg.controls_side = 0;  /* Left */
    cfg.theme         = 1;  /* Dark */
    cfg.density       = 1;  /* Standard */

    /* N.1 integration fix (2026-07-25): this wizard reads the keyboard with a
     * raw polled fb_getkey (inb 0x60). Since the A.8 interrupt fix the legacy
     * keyboard IRQ is routed through the IOAPIC and the keyboard ISR drains the
     * whole 8042 output buffer on each IRQ -- which starves the poll (bytes are
     * consumed by the ISR before fb_getkey sees them, so screens never advance).
     * Mask keyboard IRQ delivery so the poll owns the 8042 for the duration of
     * the wizard, then restore interrupt-driven delivery before returning. */
    extern int      ioapic_count(void);
    extern void     ioapic_mask(uint8_t legacy_irq);
    extern void     ioapic_set_irq(uint8_t legacy_irq, uint8_t vector, uint8_t lapic_id);
    extern uint32_t lapic_id(void);
    int kbd_irq_owned = (ioapic_count() > 0);
    if (kbd_irq_owned)
        ioapic_mask(1);

    /* Screen 1: Welcome */
#ifdef ZEOS_DIAG_N1
    kputs("[N1] screen 1/5 welcome\n");
#endif
    screen_welcome();

    /* Screen 2: Persona */
#ifdef ZEOS_DIAG_N1
    kputs("[N1] screen 2/5 persona\n");
#endif
    cfg.persona = (uint8_t)screen_persona();

    /* Screen 3: Window Controls */
#ifdef ZEOS_DIAG_N1
    kputs("[N1] screen 3/5 controls\n");
#endif
    cfg.controls_side = (uint8_t)screen_controls();

    /* Screen 4: Appearance */
#ifdef ZEOS_DIAG_N1
    kputs("[N1] screen 4/5 appearance\n");
#endif
    screen_appearance(&cfg);

    /* Screen 5: Done */
#ifdef ZEOS_DIAG_N1
    kputs("[N1] screen 5/5 done\n");
#endif
    screen_done();

    /* Mark complete so it doesn't run again */
    firstboot_mark_complete();

    /* Restore interrupt-driven keyboard delivery (vector 0x21, matches the
     * A.8 routing in main.c). */
    if (kbd_irq_owned)
        ioapic_set_irq(1, 0x21, (uint8_t)lapic_id());

    return cfg;
}
