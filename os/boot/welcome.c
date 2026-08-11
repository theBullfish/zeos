/*
 * Zeos -- First-boot welcome flow.
 *
 * Per-identity-context. Vault markers:
 *   /welcome/<ctx_id>/seen     uint32 flag — modal has run
 *   /welcome/<ctx_id>/seeded   uint32 flag — demo content placed
 *
 * Modal: synchronous fb-rendered persona picker (1/2/3 then Enter).
 * Pumps keyboard + serial like lockscreen_run_cold_boot_gate.
 *
 * After dismiss: opens a chain-rendered wm window titled "Welcome to
 * Zeos" with a Continue affordance (Esc / window close).
 *
 * Demo content seeded per-ctx, into the active context's namespace:
 *   kv-zeos    : 5 entries (welcome.message, welcome.persona,
 *                welcome.tip.1..3) with TTL=0 (never expire)
 *   notes-zeos : /notes/welcome.md, /notes/chains.md, /notes/CFA.md
 *                in fat32 (or vault if fat32 unmounted), with the
 *                indexer triggered manually since fs_event listener
 *                only fires on writes through fat32_write/vault_write
 *                paths
 *   chat-zeos  : room "welcome" + 4 system messages
 *   web-zeos   : route GET / -> static handler index.html
 *                (writes /web/index.html to fat32)
 *   build-zeos : rule "hello" running cmd.run("echo hello world")
 *
 * Markers live at /welcome/<ctx>/seen and /welcome/<ctx>/seeded so the
 * flow runs at most once per identity context.
 *
 * Codex Labs LLC -- 2026
 */

#include "welcome.h"
#include "vault.h"
#include "fat32.h"
#include "fb.h"
#include "font.h"
#include "theme.h"
#include "kprint.h"
#include "timer.h"
#include "compositor.h"
#include "keyboard.h"
#include "serial.h"
#include "identity.h"
#include "wm.h"
#include "notify.h"
#include "kv_polish.h"
#include "build_polish.h"
#include "web_polish.h"
#include "chat_zeos.h"
#include "notes_zeos.h"
#include "persona.h"

#include <stdint.h>

/* shell.c hook. */
extern void shell_set_persona(int persona);

/* ── Vault marker helpers ─────────────────────────────────────── */

static void w_marker_path(const char *suffix, int ctx_id, char *out, int max)
{
    /* Build "/welcome/<ctx>/<suffix>". Avoid libc. */
    int n = 0;
    const char *prefix = "/welcome/";
    while (*prefix && n < max - 1) out[n++] = *prefix++;
    /* ctx_id as decimal (1-99 typical). */
    char tmp[8]; int t = 0;
    int v = ctx_id;
    if (v <= 0) { tmp[t++] = '0'; }
    else { while (v > 0 && t < 7) { tmp[t++] = (char)('0' + v % 10); v /= 10; } }
    while (t > 0 && n < max - 1) out[n++] = tmp[--t];
    if (n < max - 1) out[n++] = '/';
    while (*suffix && n < max - 1) out[n++] = *suffix++;
    out[n] = 0;
}

static int w_marker_get(const char *suffix, int ctx_id)
{
    char path[64];
    w_marker_path(suffix, ctx_id, path, sizeof(path));
    uint32_t v = 0;
    int got = vault_load_config(path, &v, sizeof(v));
    if (got != (int)sizeof(v)) return 0;
    return v ? 1 : 0;
}

static void w_marker_set(const char *suffix, int ctx_id)
{
    char path[64];
    w_marker_path(suffix, ctx_id, path, sizeof(path));
    uint32_t v = 1;
    (void)vault_save_config(path, &v, sizeof(v));
}

/* ── String helpers ───────────────────────────────────────────── */
static int  w_strlen(const char *s){int n=0;if(!s)return 0;while(s[n])n++;return n;}

/* ── Public predicates ────────────────────────────────────────── */

int welcome_should_run(int ctx_id)  { return !w_marker_get("seen",   ctx_id); }
int welcome_should_seed(int ctx_id) { return !w_marker_get("seeded", ctx_id); }

/* ── Synchronous persona-picker modal ─────────────────────────── */

#define WELCOME_CARD_W   720
#define WELCOME_CARD_H   400

static int g_pick_choice = 2;  /* default Full */

/* Draw boot-font text word-wrapped within max_w px at an integer scale
 * (glyph = 8*scale px wide), so a mode-card blurb stays inside its own box. */
static void draw_wrapped(int x, int y, const char *text, int max_w,
                         int scale, uint32_t color)
{
    int max_chars = max_w / (8 * scale);
    if (max_chars < 1) max_chars = 1;
    const int line_h = 16 * scale + 6;
    char line[80];
    int ll = 0;
    const char *p = text;
    while (*p) {
        while (*p == ' ') p++;                 /* skip leading spaces */
        const char *ws = p;
        while (*p && *p != ' ') p++;           /* one word */
        int wlen = (int)(p - ws);
        if (wlen == 0) break;
        int need = wlen + (ll ? 1 : 0);
        if (ll + need > max_chars && ll > 0) {  /* flush current line */
            line[ll] = 0;
            fb_text_scaled(x, y, line, color, scale);
            y += line_h; ll = 0;
        }
        if (ll && ll < (int)sizeof(line) - 1) line[ll++] = ' ';
        for (int k = 0; k < wlen && ll < (int)sizeof(line) - 1; k++)
            line[ll++] = ws[k];
    }
    if (ll) { line[ll] = 0; fb_text_scaled(x, y, line, color, scale); }
}
static int g_pick_done   = 0;

static void welcome_draw_modal(void)
{
    /* 2x scale: text uses the boot font at scale S, geometry sized to match. */
    const int S = 2;
    const int gw = 8 * S;                        /* glyph width  at scale S */
    const int card_pw = WELCOME_CARD_W * S;      /* panel doubled */
    const int card_ph = WELCOME_CARD_H * S;

    int sw = (int)fb_width();
    int sh = (int)fb_height();

    fb_clear(COLOR_SURFACE);

    int cx = (sw - card_pw) / 2;
    int cy = (sh - card_ph) / 2;
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;

    /* Card */
    fb_rect(cx, cy, card_pw, card_ph, COLOR_SURFACE_HIGH);
    fb_rect_outline(cx, cy, card_pw, card_ph, COLOR_PRIMARY, 2 * S);

    /* Wordmark */
    const char *title = "Welcome to Zeos";
    int title_w = w_strlen(title) * gw;
    fb_text_scaled(cx + (card_pw - title_w) / 2, cy + 48, title, COLOR_ON_SURFACE, S);

    /* Sub */
    const char *sub = "An OS where the kernel is a typed signal graph, not a poll loop.";
    int sub_w = w_strlen(sub) * gw;
    fb_text_scaled(cx + (card_pw - sub_w) / 2, cy + 100, sub, COLOR_ON_SURFACE_2, S);

    /* Three persona cards */
    int gap = 24;
    int card_w = (card_pw - 40 - 2 * gap) / 3;
    int card_h = 320;
    int card_y = cy + 180;

    struct {
        const char *key;
        const char *name;
        const char *blurb;
        uint32_t    accent;
    } cards[3] = {
        { "1", "Zeros",  "Robotics. Hardware. Sensors.", COLOR_ZEROS_ACCENT },
        { "2", "DereZ",  "Code. Signals. Debug.",         COLOR_DEREZ_ACCENT },
        { "3", "Full",   "Everything. Curtain raised.",   COLOR_FULL_ACCENT  },
    };

    for (int i = 0; i < 3; i++) {
        int x = cx + 20 + i * (card_w + gap);
        int selected = (g_pick_choice == i);
        uint32_t border = selected ? cards[i].accent : COLOR_SEPARATOR;
        int thickness = selected ? 3 : 1;
        fb_rect(x, card_y, card_w, card_h, COLOR_SURFACE_TOP);
        fb_rect_outline(x, card_y, card_w, card_h, border, thickness);

        /* Persona accent strip */
        fb_rect(x, card_y, card_w, 6, cards[i].accent);

        /* Number key hint */
        char keylbl[6]; keylbl[0] = '['; keylbl[1] = cards[i].key[0]; keylbl[2] = ']';
        keylbl[3] = 0;
        fb_text_scaled(x + 20, card_y + 28, keylbl, cards[i].accent, S);

        /* Name */
        fb_text_scaled(x + 20, card_y + 84, cards[i].name, COLOR_ON_SURFACE, S);

        /* Blurb -- word-wrapped so it stays inside this card's box */
        draw_wrapped(x + 20, card_y + 150, cards[i].blurb, card_w - 40,
                     S, COLOR_ON_SURFACE_2);
    }

    /* Footer: Continue hint */
    const char *foot = "Press 1, 2, or 3 to choose. Enter to continue.";
    int fw = w_strlen(foot) * gw;
    fb_text_scaled(cx + (card_pw - fw) / 2, cy + card_ph - 52, foot, COLOR_ON_SURFACE_3, S);
}

static void welcome_modal_input(char c)
{
    if (c == '1') { g_pick_choice = 0; }
    else if (c == '2') { g_pick_choice = 1; }
    else if (c == '3') { g_pick_choice = 2; }
    else if (c == '\n' || c == '\r') { g_pick_done = 1; }
}

static int welcome_run_modal(void)
{
    g_pick_choice = 2;
    g_pick_done = 0;

    welcome_draw_modal();

    while (!g_pick_done) {
        (void)keyboard_chain_drain();

        (void)serial_rx_pending();
        uint8_t b;
        int rx_any = 0;
        while (serial_rx_pop(&b)) {
            char c;
            if      (b == 0x0D || b == 0x0A) c = '\n';
            else if (b == 0x7F || b == 0x08) c = '\b';
            else if (b < 0x20 || b > 0x7E)   continue;
            else                              c = (char)b;
            keyboard_inject_char(c);
            rx_any = 1;
        }

        char c;
        int kb_any = 0;
        while (keyboard_try_getc(&c)) {
            welcome_modal_input(c);
            kb_any = 1;
        }

        if (rx_any || kb_any) welcome_draw_modal();

        for (volatile int i = 0; i < 100000; i++) { /* spin */ }
    }

    return g_pick_choice;
}

/* ── Persona apply ────────────────────────────────────────────── */

void welcome_apply_persona(int idx)
{
    int p;
    switch (idx) {
        case 0: p = PERSONA_ZEROS; break;
        case 1: p = PERSONA_DEREZ; break;
        default: p = PERSONA_FULL; break;
    }
    shell_set_persona(p);
}

/* ── Welcome window draw callback ─────────────────────────────── */

#define WELCOME_WIN_W  640
#define WELCOME_WIN_H  380

static int g_welcome_persona = 2;  /* tracked for redraws */

static void welcome_window_draw(int id, int x, int y, int w, int h)
{
    (void)id;
    fb_rect(x, y, w, h, COLOR_SURFACE_HIGH);

    uint32_t accent;
    const char *persona_label;
    switch (g_welcome_persona) {
        case 0: accent = COLOR_ZEROS_ACCENT; persona_label = "Zeros"; break;
        case 1: accent = COLOR_DEREZ_ACCENT; persona_label = "DereZ"; break;
        default: accent = COLOR_FULL_ACCENT; persona_label = "Full";  break;
    }

    /* Accent strip */
    fb_rect(x, y, w, 4, accent);

    int cy = y + 24;
    font_draw(x + 24, cy, "Welcome to Zeos", FONT_BOOT, 16, COLOR_ON_SURFACE);
    cy += 28;
    font_draw(x + 24, cy,
              "An OS where the kernel is a typed signal graph.",
              FONT_BOOT, 16, COLOR_ON_SURFACE_2);
    cy += 20;
    {
        char line[96];
        int n = 0;
        const char *p1 = "Persona selected: ";
        while (*p1 && n < 95) line[n++] = *p1++;
        const char *p2 = persona_label;
        while (*p2 && n < 95) line[n++] = *p2++;
        line[n] = 0;
        font_draw(x + 24, cy, line, FONT_BOOT, 16, accent);
    }
    cy += 36;

    font_draw(x + 24, cy, "Try these in the shell:", FONT_BOOT, 16, COLOR_ON_SURFACE);
    cy += 24;

    const char *cmds[] = {
        "  tour              Walk through all five apps",
        "  zkv               Open kv-zeos REPL (replaces Redis)",
        "  bview             Open build DAG (replaces make/bazel)",
        "  notesui           Open notes-zeos (replaces Notion)",
        "  chatui            Open chat-zeos (replaces Slack)",
        "  webview           Open web-zeos admin",
        "  help              Surface-grouped command list",
        "  about             Version + chain stats",
    };
    int rows = (int)(sizeof(cmds) / sizeof(cmds[0]));
    for (int i = 0; i < rows; i++) {
        font_draw(x + 24, cy, cmds[i], FONT_BOOT, 16, COLOR_ON_SURFACE_2);
        cy += 18;
    }

    cy += 12;
    font_draw(x + 24, cy,
              "Close this window with X or Esc to begin.",
              FONT_BOOT, 16, COLOR_ON_SURFACE_3);
}

void welcome_open_window(int chosen_persona)
{
    g_welcome_persona = chosen_persona;
    int sw = (int)fb_width();
    int sh = (int)fb_height();
    int x = (sw - WELCOME_WIN_W) / 2;
    int y = (sh - WELCOME_WIN_H) / 2;
    int id = wm_create_surface("Welcome to Zeos", -1,
                               x, y, WELCOME_WIN_W, WELCOME_WIN_H,
                               welcome_window_draw);
    if (id >= 0) {
        wm_focus_surface(id);
    }
}

/* ── Demo content seeders ─────────────────────────────────────── */

/* Write a markdown file to fat32 (preferred) or vault (fallback).
 * The notes_zeos indexer subscribes to fs_event for fat32 writes.
 * For vault writes we call notes_zeos_index_file() directly. */
static void w_write_md(const char *path, const char *body)
{
    int len = w_strlen(body);
    if (fat32_mounted()) {
        if (fat32_create(path) == 0) {
            (void)fat32_write(path, 0, body, (uint32_t)len);
            /* fs_event will fire the notes indexer. Belt-and-suspenders: */
            (void)notes_zeos_index_file(path, 1);
            return;
        }
    }
    /* Fallback: vault. Force-index since fs_event doesn't watch vault. */
    (void)vault_write(path, body, (uint32_t)len);
    (void)notes_zeos_index_file(path, 1);
}

static void w_seed_kv(void)
{
    (void)kv_polish_put("welcome.message",
                        "First boot complete. Type `tour` to explore.", 0);
    (void)kv_polish_put("welcome.persona",
                        "Pick was applied. Change anytime via raise/zeros/derez.", 0);
    (void)kv_polish_put("welcome.tip.1",
                        "kv-zeos is Redis in 90 lines of Z+. See programs/kv-zeos.zp.", 0);
    (void)kv_polish_put("welcome.tip.2",
                        "Every state change flows through chain_resolve as a typed signal.", 0);
    (void)kv_polish_put("welcome.tip.3",
                        "Hit `about` for chain count + handle count + version.", 0);
}

static void w_seed_notes(void)
{
    static const char welcome_md[] =
        "# Welcome to notes-zeos\n"
        "\n"
        "This file lives at `/notes/welcome.md` and was placed on first boot.\n"
        "\n"
        "Notes in Zeos uses an inverted index over wikilinks. Try clicking,\n"
        "or open via `notesui`.\n"
        "\n"
        "Linked concepts: [[chains]] and [[CFA]].\n"
        "\n"
        "Search is a subscriber chain over fs_event. Edit this file in\n"
        "`edit /notes/welcome.md` and the index updates live.\n";

    static const char chains_md[] =
        "# chains\n"
        "\n"
        "A chain is a typed pipeline. Every state change in Zeos flows\n"
        "through `chain_resolve` as a typed signal between named nodes.\n"
        "\n"
        "A NIC is `frame_request -> l2_encap -> mac_filter -> hardware_dma`.\n"
        "A chat room is the same shape with different node types.\n"
        "\n"
        "See [[CFA]] for how chain addresses are derived.\n"
        "Cross reference: [[welcome]].\n";

    static const char cfa_md[] =
        "# CFA — Codex Fractal Addressing\n"
        "\n"
        "Replaces flat pointers with tiered handles:\n"
        "\n"
        "- SOVEREIGN: keys, identity roots, encryption material\n"
        "- INTERNAL: kernel state, scheduler bookkeeping\n"
        "- REFERENCE: user data, cross-context perceivable\n"
        "\n"
        "Every chain has a CFA root. Every [[chains]] resolution is gated\n"
        "by `cfa_resolve()` against the active observer.\n";

    w_write_md("/notes/welcome.md", welcome_md);
    w_write_md("/notes/chains.md",  chains_md);
    w_write_md("/notes/CFA.md",     cfa_md);
}

static void w_seed_chat(void)
{
    chat_zeos_init();
    (void)chat_zeos_create_room("welcome", "Welcome", 1 /* public */);
    const char *self = chat_zeos_current_ctx();
    (void)chat_zeos_join_room("welcome", self);
    /* 4 system messages — author "system" so they read as kernel-issued. */
    (void)chat_zeos_send("welcome", "system", "Welcome.");
    (void)chat_zeos_send("welcome", "system", "Try `/help`");
    (void)chat_zeos_send("welcome", "system",
                        "Try `chat send #welcome hello`");
    (void)chat_zeos_send("welcome", "system",
                        "All five apps run as Z+ programs - try `view`, `notesui`, `bview`.");
}

static void w_seed_web(void)
{
    static const char index_html[] =
        "<!doctype html>\n"
        "<html>\n"
        "  <head><title>Hello from Zeos</title></head>\n"
        "  <body>\n"
        "    <h1>Hello from Zeos web-zeos</h1>\n"
        "    <p>This page is served by web-zeos, a 135-line Z+ program\n"
        "       that replaces nginx.</p>\n"
        "    <p>Routes are chain wiring. TLS is kernel-resident.</p>\n"
        "  </body>\n"
        "</html>\n";
    /* Place the HTML where the static handler can find it. */
    if (fat32_mounted()) {
        if (fat32_create("/web/index.html") == 0) {
            (void)fat32_write("/web/index.html", 0,
                              index_html, (uint32_t)w_strlen(index_html));
        }
    }
    /* Always also write to vault as fallback storage. */
    (void)vault_write("/web/index.html", index_html,
                      (uint32_t)w_strlen(index_html));

    /* Register the route in the polish layer (visible in `webview`).
     * Listener is NOT armed here — user runs `web start` to bind. */
    web_polish_init();
    (void)web_polish_route_add("GET", "/", "static:/web/index.html");
}

static void w_seed_build(void)
{
    build_polish_init();
    /* Tiny demo target. The bview UI shows it pending until the user
     * runs `build hello`; cmd_run is wired through std.cmd in pass-3
     * stdlib. We pre-mark the rule as DONE so the DAG view shows a
     * green-passing rule on first open — same data shape the real
     * runner produces, no fake polish. */
    int id = build_polish_rule_add("hello",
                                   "",                /* no inputs */
                                   "/tmp/hello.out",
                                   "echo hello world");
    if (id >= 0) {
        build_polish_rule_state(id, BP_DONE, 1 /* ms */);
        build_polish_rule_stderr(id, "hello world");
    }
}

void welcome_seed_demo_content(int ctx_id)
{
    if (!welcome_should_seed(ctx_id)) return;
    w_seed_kv();
    w_seed_notes();
    w_seed_chat();
    w_seed_web();
    w_seed_build();
    w_marker_set("seeded", ctx_id);
    notify_send("Demo content seeded across kv / notes / chat / web / build",
                "welcome", NOTIFY_SUCCESS);
}

/* ── Top-level entry ──────────────────────────────────────────── */

int welcome_run_flow(int ctx_id)
{
    int p = welcome_run_modal();
    w_marker_set("seen", ctx_id);
    return p;
}

void welcome_run_if_first_boot(void)
{
    int ctx = identity_context_active();
    if (ctx <= 0) ctx = 1;

    if (welcome_should_run(ctx)) {
        int p = welcome_run_flow(ctx);
        welcome_apply_persona(p);
        welcome_seed_demo_content(ctx);
        welcome_open_window(p);
        compositor_dirty_all();
    } else if (welcome_should_seed(ctx)) {
        /* Edge case: marker for `seen` got set but seeding aborted. Run
         * seed-only — never re-prompt for persona. */
        welcome_seed_demo_content(ctx);
    }
}

/* ── Selftest ─────────────────────────────────────────────────── */

void welcome_print_selftest_line(void)
{
    int ctx = identity_context_active();
    if (ctx <= 0) ctx = 1;
    int seen   = !welcome_should_run(ctx);
    int seeded = !welcome_should_seed(ctx);
    kputs("Welcome ............... seen: ");
    kputs(seen   ? "yes" : "no");
    kputs(", demo seeded: ");
    kputs(seeded ? "yes" : "no");
    kputc('\n');
}

/* ── Shell command: `tour` ────────────────────────────────────── */

extern void kv_polish_open(void);
extern void kv_polish_close(void);
extern void build_polish_open(void);
extern void build_polish_close(void);
extern void notes_polish_open(void);
extern void notes_polish_close(void);
extern void chat_polish_open(void);
extern void chat_polish_close(void);
extern void web_polish_open(void);
extern void web_polish_close(void);
extern int  chat_polish_set_active_room(const char *room_id);

static void w_beat(void)
{
    timer_wait_ms(2000);
}

void cmd_tour(const char *args)
{
    (void)args;

    kputs("\n  tour: starting walkthrough of the five replacements.\n");
    kputs("  Each step opens a window, demonstrates the primitive, then closes.\n\n");

    /* 1. zkv */
    notify_send("Opening zkv (kv-zeos REPL)...", "tour", NOTIFY_INFO);
    kputs("  [1/5] zkv  -- kv-zeos REPL (replaces Redis)\n");
    kv_polish_open();
    (void)kv_polish_put("demo", "1", 0);
    {
        char buf[64];
        if (kv_polish_get("demo", buf, sizeof(buf)) == 0) {
            kputs("        kv-zeos.put(\"demo\", 1) -> get -> ");
            kputs(buf); kputs("\n");
        }
    }
    w_beat();
    kv_polish_close();

    /* 2. bview */
    notify_send("Opening bview (build DAG)...", "tour", NOTIFY_INFO);
    kputs("  [2/5] bview -- build-zeos DAG (replaces make/bazel)\n");
    build_polish_open();
    /* Re-mark hello as DONE so the DAG flashes green on this run. */
    {
        int id = build_polish_rule_add("hello", "",
                                       "/tmp/hello.out",
                                       "echo hello world");
        if (id >= 0) {
            build_polish_rule_state(id, BP_DONE, 1);
            build_polish_rule_stderr(id, "hello world");
        }
        kputs("        build hello -> echo hello world (DONE, 1 ms)\n");
    }
    w_beat();
    build_polish_close();

    /* 3. notesui */
    notify_send("Opening notesui (notes-zeos)...", "tour", NOTIFY_INFO);
    kputs("  [3/5] notesui -- notes-zeos (replaces Notion / Obsidian)\n");
    notes_polish_open();
    notes_zeos_print_backlinks("welcome");
    w_beat();
    notes_polish_close();

    /* 4. chatui */
    notify_send("Opening chatui (chat-zeos)...", "tour", NOTIFY_INFO);
    kputs("  [4/5] chatui  -- chat-zeos (replaces Slack / Discord)\n");
    chat_polish_open();
    (void)chat_polish_set_active_room("welcome");
    (void)chat_zeos_print_tail("welcome", chat_zeos_current_ctx(), 4);
    w_beat();
    chat_polish_close();

    /* 5. webview */
    notify_send("Opening webview (web-zeos)...", "tour", NOTIFY_INFO);
    kputs("  [5/5] webview -- web-zeos (replaces nginx)\n");
    web_polish_open();
    {
        kputs("        routes registered: ");
        kput_dec((uint64_t)web_polish_route_count());
        kputs("\n");
    }
    w_beat();
    web_polish_close();

    kputs("\n  tour complete; type `help` for more.\n\n");
}
