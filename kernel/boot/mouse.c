/*
 * Zeos — PS/2 mouse driver
 *
 * Uses the 8042 keyboard controller's auxiliary port (port 0x60/0x64)
 * to communicate with a PS/2 mouse. Decodes standard 3-byte packets
 * and feeds absolute position + button events to the cursor system.
 *
 * PS/2 mouse protocol:
 *   Byte 0: [Y-ovf][X-ovf][Y-sign][X-sign][1][Middle][Right][Left]
 *   Byte 1: X movement (unsigned, sign in byte 0 bit 4)
 *   Byte 2: Y movement (unsigned, sign in byte 0 bit 5)
 *
 * Initialization sequence:
 *   1. Enable auxiliary device on 8042 controller
 *   2. Send 0xFF (reset) to mouse, wait for 0xAA (self-test pass)
 *   3. Send 0xF6 (set defaults) — 100 samples/sec, 4 counts/mm
 *   4. Send 0xF4 (enable data reporting)
 *   5. Register IRQ12 handler (vector 0x2C)
 */

#include "mouse.h"
#include "idt.h"
#include "io.h"
#include "fb.h"
#include "cursor.h"
#include "idle.h"
#include "ui_hover.h"
#include "ui_context_menu.h"
#include "ui_dirty.h"
#include "quick_look.h"
#include "image_viewer.h"
#include "wm.h"

/* ── Forward: dispatch button events to UI overlays.
 * Returns 1 if the event was consumed by an overlay (caller should
 * suppress further routing). button: 1=left, 2=right, 3=middle. */
/* Forward declarations of per-app right-click hooks. Each returns 1 if
 * it consumed the event; we walk them in priority order before the
 * generic fallback menu. */
extern int  panel_right_click(int x, int y);
extern int  dock_right_click(int x, int y);
extern int  inspector_right_click(int x, int y);
extern int  palette_right_click(int x, int y);
extern int  desktop_right_click(int x, int y);

/* While a pre-scheduler modal (welcome picker / cold-boot lock) owns input,
 * swallow mouse clicks so a stray USB-HID mouse event can't reach wm/desktop
 * underneath the modal. Cleared once scheduler_run owns input. */
static int s_input_modal;
void mouse_set_modal(int on) { s_input_modal = on ? 1 : 0; }

static int mouse_ui_dispatch_down(int x, int y, int button) {
    if (s_input_modal) return 1;   /* consume; modal owns input */
    extern int workspaces_overview_active(void);
    extern int workspaces_overview_mouse_down(int x, int y, int button);
    if (workspaces_overview_active() &&
        workspaces_overview_mouse_down(x, y, button)) return 1;
    if (dirty_modal_active() && dirty_modal_mouse_down(x, y, button)) return 1;
    {
        extern int calculator_active(void);
        extern int calculator_mouse_down(int, int, int);
        if (calculator_active() && calculator_mouse_down(x, y, button)) return 1;
    }
    {
        extern int editor_active(void);
        extern int editor_mouse_down(int, int, int);
        if (editor_active() && editor_mouse_down(x, y, button)) return 1;
    }
    {
        extern int file_mgr_active(void);
        extern int file_mgr_mouse_down(int, int, int);
        if (file_mgr_active() && file_mgr_mouse_down(x, y, button)) return 1;
    }
    {
        extern int activity_active(void);
        extern int activity_mouse_down(int, int, int);
        if (activity_active() && activity_mouse_down(x, y, button)) return 1;
    }
    if (image_viewer_active() && image_viewer_mouse_down(x, y, button)) return 1;
    if (quick_look_active()  && quick_look_mouse_down(x, y, button))  return 1;
    if (context_menu_active() && context_menu_mouse_down(x, y, button)) return 1;
    if (button == 1) {
        /* Left-click routing: panel/dock chrome, then the WM (full-frame hit
         * test incl. titlebar/controls/resize edges -- so focus/drag/close/
         * resize all work), then the desktop as the fall-through. */
        extern int  panel_left_click(int x, int y);
        extern int  dock_left_click(int x, int y);
        extern void desktop_click(int x, int y);
        extern void compositor_dirty_all(void);
        compositor_dirty_all();         /* any click changes focus/state -> recomposite */
        if (panel_left_click(x, y)) return 1;
        if (dock_left_click(x, y))  return 1;
        if (wm_mouse_down(x, y, 1) >= 0) return 1;
        desktop_click(x, y);
        return 1;
    }
    if (button == 2) {
        /* App-level handlers first — most specific wins. */
        if (palette_right_click(x, y))   return 1;
        if (panel_right_click(x, y))     return 1;
        if (dock_right_click(x, y))      return 1;
        if (inspector_right_click(x, y)) return 1;
        /* WM-routed per-window right-click: find the topmost window
         * whose content area contains (x,y), translate to window-local
         * coords, and dispatch to its on_right_click callback. */
        {
            int wid = wm_window_at(x, y);
            if (wid >= 0) {
                int lx, ly;
                if (wm_screen_to_window(wid, x, y, &lx, &ly)) {
                    chain_surface_t *cs = wm_get_surface(wid);
                    if (cs && cs->on_right_click) {
                        cs->on_right_click(lx, ly);
                        return 1;
                    }
                }
            }
        }
        if (desktop_right_click(x, y))   return 1;
        /* Generic fallback menu — shouldn't normally be reached now. */
        static ctx_menu_item_t fallback[] = {
            { "Refresh",   0, 0, 1 },
            { "Copy",      0, 0, 0 },
            { "Paste",     0, 0, 0 },
            { "-",         0, 0, 1 },
            { "Properties",0, 0, 1 },
        };
        context_menu_open(x, y, fallback, 5);
        return 1;
    }
    return 0;
}

/* Shared left-release handler so PS/2 and USB drop a drag identically. */
static void mouse_ui_dispatch_up(int x, int y, int button) {
    if (button == 1) {
        extern void desktop_drag_end(int x, int y);
        extern void compositor_dirty_all(void);
        wm_mouse_up(x, y, 1);       /* commit snap / clear ghost / stop drag */
        desktop_drag_end(x, y);
        compositor_dirty_all();     /* redraw the settled result */
    }
}

static void mouse_ui_dispatch_move(int x, int y) {
    extern int workspaces_overview_active(void);
    extern int workspaces_overview_mouse_move(int x, int y);
    if (workspaces_overview_active()) {
        workspaces_overview_mouse_move(x, y);
        return;
    }
    if (dirty_modal_active())  { dirty_modal_mouse_move(x, y); return; }
    {
        extern int calculator_active(void);
        extern int calculator_mouse_move(int, int);
        if (calculator_active()) { calculator_mouse_move(x, y); /* fall through to hover_dispatch */ }
    }
    {
        extern int editor_active(void);
        extern int editor_mouse_move(int, int);
        if (editor_active()) { editor_mouse_move(x, y); /* fall through to hover_dispatch */ }
    }
    {
        extern int file_mgr_active(void);
        extern int file_mgr_mouse_move(int, int);
        if (file_mgr_active()) { file_mgr_mouse_move(x, y); }
    }
    {
        extern int activity_active(void);
        extern int activity_mouse_move(int, int);
        if (activity_active()) { activity_mouse_move(x, y); }
    }
    if (image_viewer_active()) { image_viewer_mouse_move(x, y); return; }
    if (context_menu_active()) { context_menu_mouse_move(x, y); /* still hover the rest */ }
    /* Drive window drag/resize while the left button is held (wm_mouse_move
     * self-gates on s->dragging/s->resizing, so it's a no-op otherwise). */
    if (mouse_get_buttons() & MOUSE_BTN_LEFT) {
        extern void wm_mouse_move(int x, int y);
        wm_mouse_move(x, y);
    }
    hover_dispatch(x, y);
}

/* ── 8042 controller ports ── */
#define PS2_DATA_PORT    0x60
#define PS2_STATUS_PORT  0x64
#define PS2_CMD_PORT     0x64

/* ── 8042 status register bits ── */
#define PS2_STATUS_OBF   (1 << 0)  /* Output buffer full (data ready) */
#define PS2_STATUS_IBF   (1 << 1)  /* Input buffer full (controller busy) */
#define PS2_STATUS_AUX   (1 << 5)  /* Aux data (mouse vs keyboard) */

/* ── 8042 controller commands ── */
#define PS2_CMD_READ_CONFIG   0x20
#define PS2_CMD_WRITE_CONFIG  0x60
#define PS2_CMD_DISABLE_AUX   0xA7
#define PS2_CMD_ENABLE_AUX    0xA8
#define PS2_CMD_TEST_AUX      0xA9
#define PS2_CMD_WRITE_AUX     0xD4  /* Next byte goes to mouse */

/* ── Mouse commands (sent via 0xD4) ── */
#define MOUSE_CMD_RESET       0xFF
#define MOUSE_CMD_RESEND      0xFE
#define MOUSE_CMD_SET_DEFAULT  0xF6
#define MOUSE_CMD_DISABLE     0xF5
#define MOUSE_CMD_ENABLE      0xF4
#define MOUSE_CMD_SET_RATE    0xF3
#define MOUSE_CMD_GET_ID      0xF2

/* ── Mouse responses ── */
#define MOUSE_ACK             0xFA
#define MOUSE_SELF_TEST_PASS  0xAA

/* ── Packet state machine ── */
static volatile int packet_index;
static uint8_t packet[3];

/* ── Mouse state ── */
static mouse_state_t g_mouse;

/* Raw packet ring for CHAIN_MOUSE. The IRQ assembles 3-byte packets
 * and pushes them here; the chain resolve drains the ring through
 * process_packet on each scheduler tick so mouse events flow through
 * the same MasQ/B3 path as every other device. */
#define MS_PKT_RING 128
static volatile uint8_t  ms_pkt_ring[MS_PKT_RING][3];
static volatile uint32_t ms_pkt_head;
static volatile uint32_t ms_pkt_tail;
static volatile uint32_t ms_pkt_drained;
static volatile uint32_t ms_pending_irqs;

static void ms_pkt_push(const uint8_t *p)
{
    uint32_t next = (ms_pkt_head + 1) % MS_PKT_RING;
    if (next != ms_pkt_tail) {
        ms_pkt_ring[ms_pkt_head][0] = p[0];
        ms_pkt_ring[ms_pkt_head][1] = p[1];
        ms_pkt_ring[ms_pkt_head][2] = p[2];
        ms_pkt_head = next;
        ms_pending_irqs++;
    }
}

int mouse_chain_pending(void)
{
    return (int)((ms_pkt_head - ms_pkt_tail) & (MS_PKT_RING - 1));
}

uint32_t mouse_chain_total(void)
{
    return ms_pkt_drained;
}

uint32_t mouse_pending_irqs(void)
{
    return ms_pending_irqs;
}

/* ── Forward declarations ── */
static void mouse_isr(uint64_t vector, uint64_t error_code);
static void process_packet(void);

/*
 * Wait until the 8042 input buffer is empty (safe to write).
 * Timeout after ~100ms worth of port reads.
 */
static void ps2_wait_write(void)
{
    int timeout = 100000;
    while (timeout-- > 0) {
        if (!(inb(PS2_STATUS_PORT) & PS2_STATUS_IBF))
            return;
    }
}

/*
 * Wait until the 8042 output buffer has data (safe to read).
 * Timeout after ~100ms worth of port reads.
 */
static void ps2_wait_read(void)
{
    int timeout = 100000;
    while (timeout-- > 0) {
        if (inb(PS2_STATUS_PORT) & PS2_STATUS_OBF)
            return;
    }
}

/*
 * Send a command byte to the 8042 controller.
 */
static void ps2_cmd(uint8_t cmd)
{
    ps2_wait_write();
    outb(PS2_CMD_PORT, cmd);
}

/*
 * Send a data byte to the 8042 data port.
 */
static void ps2_data_write(uint8_t data)
{
    ps2_wait_write();
    outb(PS2_DATA_PORT, data);
}

/*
 * Read a byte from the 8042 data port.
 */
static uint8_t ps2_data_read(void)
{
    ps2_wait_read();
    return inb(PS2_DATA_PORT);
}

/*
 * Send a command to the mouse (via 8042 auxiliary port).
 * Waits for ACK (0xFA) from the mouse.
 */
static void mouse_write(uint8_t cmd)
{
    ps2_cmd(PS2_CMD_WRITE_AUX);
    ps2_data_write(cmd);

    /* Wait for ACK — mouse responds with 0xFA */
    int timeout = 100000;
    while (timeout-- > 0) {
        if (inb(PS2_STATUS_PORT) & PS2_STATUS_OBF) {
            uint8_t resp = inb(PS2_DATA_PORT);
            if (resp == MOUSE_ACK)
                return;
        }
    }
}

/*
 * Flush any pending data from the 8042 output buffer.
 */
static void ps2_flush(void)
{
    int timeout = 1000;
    while (timeout-- > 0) {
        if (inb(PS2_STATUS_PORT) & PS2_STATUS_OBF)
            inb(PS2_DATA_PORT);
        else
            break;
    }
}

/*
 * Initialize PS/2 mouse.
 */
void mouse_init(void)
{
    /* Clear state */
    g_mouse.x = (int)fb_width() / 2;   /* Start at screen center */
    g_mouse.y = (int)fb_height() / 2;
    g_mouse.dx = 0;
    g_mouse.dy = 0;
    g_mouse.buttons = 0;
    g_mouse.prev_buttons = 0;
    packet_index = 0;

    /* Flush any stale data */
    ps2_flush();

    /* Enable the auxiliary (mouse) port on the 8042 */
    ps2_cmd(PS2_CMD_ENABLE_AUX);

    /* Read current config byte */
    ps2_cmd(PS2_CMD_READ_CONFIG);
    uint8_t config = ps2_data_read();

    /* Enable auxiliary interrupt (bit 1) and auxiliary clock (clear bit 5) */
    config |= (1 << 1);   /* Enable IRQ12 */
    config &= ~(1 << 5);  /* Enable auxiliary clock */

    ps2_cmd(PS2_CMD_WRITE_CONFIG);
    ps2_data_write(config);

    /* Test auxiliary port (optional, for diagnostics) */
    ps2_cmd(PS2_CMD_TEST_AUX);
    ps2_data_read();  /* 0x00 = pass */

    /* Reset mouse — sends 0xFF, expects 0xAA then 0x00 (device ID) */
    mouse_write(MOUSE_CMD_RESET);
    /* Read self-test result (0xAA) and device ID (0x00) */
    ps2_data_read();  /* 0xAA self-test pass */
    ps2_data_read();  /* 0x00 device ID */

    /* Set defaults (100 samples/sec, 4 counts/mm, scaling 1:1) */
    mouse_write(MOUSE_CMD_SET_DEFAULT);

    /* Set sample rate to 100/sec */
    mouse_write(MOUSE_CMD_SET_RATE);
    mouse_write(100);

    /* Enable data reporting — mouse starts sending packets */
    mouse_write(MOUSE_CMD_ENABLE);

    /* Flush any leftover data from init handshake */
    ps2_flush();

    /* Register IRQ12 handler (IRQ12 = vector 0x2C on remapped PIC) */
    idt_register(0x2C, mouse_isr);

    /* Unmask IRQ12 on the slave PIC */
    pic_unmask(12);
}

/*
 * IRQ12 handler — called on every mouse data byte.
 * Assembles 3-byte packets and processes them.
 */
static void mouse_isr(uint64_t vector, uint64_t error_code)
{
    (void)vector;
    (void)error_code;
    extern void lapic_eoi(void);
    extern uint32_t g_legacy_irq_fire_count; g_legacy_irq_fire_count++;  /* TEMP-INSTR A.8 */

    uint8_t status = inb(PS2_STATUS_PORT);

    /* Explicit EOI on every path (defensive -- isr_dispatch in idt.c ALSO
     * auto-EOIs the 8259 for every 0x20-0x2F vector regardless of what the
     * handler does, so 8259-level EOI was never actually missing here).
     * lapic_eoi() added 2026-07-23 on the theory that the LAPIC's own
     * in-service bit needs clearing too (scheduler.c's timer ISR, which
     * fires reliably every tick, always calls it) -- NOT CONFIRMED to fix
     * the real symptom (see BUILD_MAP/bible-db E.1: real mouse/keyboard
     * interrupts still fire exactly once, ever, and never again, unchanged
     * after this addition). Root cause of that is still open. */
    if (!(status & PS2_STATUS_OBF) || !(status & PS2_STATUS_AUX)) {
        pic_eoi(12);
        lapic_eoi();
        return;
    }

    uint8_t data = inb(PS2_DATA_PORT);

    switch (packet_index) {
    case 0:
        /* Byte 0: status byte — bit 3 must always be set */
        if (data & (1 << 3)) {
            packet[0] = data;
            packet_index = 1;
        }
        /* If bit 3 is not set, we're out of sync — discard and resync */
        break;

    case 1:
        /* Byte 1: X movement */
        packet[1] = data;
        packet_index = 2;
        break;

    case 2:
        /* Byte 2: Y movement — packet complete. Push to ring; the
         * CHAIN_MOUSE resolve drains and decodes on the next tick. */
        packet[2] = data;
        packet_index = 0;
        ms_pkt_push(packet);
        break;
    }

    /* Send EOI to slave PIC (IRQ >= 8) then master, then the LAPIC. */
    pic_eoi(12);
    lapic_eoi();
}

uint32_t mouse_chain_drain(void)
{
    uint32_t n = 0;
    while (ms_pkt_tail != ms_pkt_head) {
        packet[0] = ms_pkt_ring[ms_pkt_tail][0];
        packet[1] = ms_pkt_ring[ms_pkt_tail][1];
        packet[2] = ms_pkt_ring[ms_pkt_tail][2];
        ms_pkt_tail = (ms_pkt_tail + 1) % MS_PKT_RING;
        process_packet();
        n++;
        ms_pkt_drained++;
    }
    return n;
}

/*
 * Decode a complete 3-byte mouse packet and update state.
 *
 * Byte 0 bits:
 *   [7] Y overflow    [6] X overflow
 *   [5] Y sign        [4] X sign
 *   [3] Always 1      [2] Middle btn  [1] Right btn  [0] Left btn
 *
 * Byte 1: X delta (unsigned, sign-extended via byte 0 bit 4)
 * Byte 2: Y delta (unsigned, sign-extended via byte 0 bit 5)
 */
static void process_packet(void)
{
    uint8_t status = packet[0];

    /* Discard overflow packets */
    if (status & ((1 << 6) | (1 << 7)))
        return;

    /* Any packet -- movement or button -- is an input event for the
     * idle/lock state machine. Mark active before decoding so a click
     * on a BLANKED screen wakes us straight to the lock prompt. */
    idle_mark_active();

    /* Sign-extend deltas */
    int dx = (int)packet[1];
    int dy = (int)packet[2];

    if (status & (1 << 4))
        dx |= ~0xFF;  /* sign-extend: X is negative */
    if (status & (1 << 5))
        dy |= ~0xFF;  /* sign-extend: Y is negative */

    /* PS/2 Y axis is inverted (positive = up), screen Y = down */
    dy = -dy;

    /* Store deltas */
    g_mouse.dx = dx;
    g_mouse.dy = dy;

    /* Accumulate into absolute position */
    g_mouse.x += dx;
    g_mouse.y += dy;

    /* Clamp to screen bounds */
    int w = (int)fb_width();
    int h = (int)fb_height();

    if (g_mouse.x < 0) g_mouse.x = 0;
    if (g_mouse.y < 0) g_mouse.y = 0;
    if (g_mouse.x >= w) g_mouse.x = w - 1;
    if (g_mouse.y >= h) g_mouse.y = h - 1;

    /* Update button state */
    g_mouse.prev_buttons = g_mouse.buttons;
    g_mouse.buttons = status & 0x07;  /* bits [2:0] = middle, right, left */

    /* Feed cursor system */
    cursor_move(g_mouse.x, g_mouse.y);

    /* UI hover dispatch on every move (cheap; static array walk). */
    mouse_ui_dispatch_move(g_mouse.x, g_mouse.y);

    /* Detect button edges and fire cursor press/release */
    uint8_t changed = g_mouse.buttons ^ g_mouse.prev_buttons;

    if (changed & MOUSE_BTN_LEFT) {
        if (g_mouse.buttons & MOUSE_BTN_LEFT) {
            mouse_ui_dispatch_down(g_mouse.x, g_mouse.y, 1);
            cursor_press();
        } else {
            if (image_viewer_active())
                image_viewer_mouse_up(g_mouse.x, g_mouse.y, 1);
            mouse_ui_dispatch_up(g_mouse.x, g_mouse.y, 1);
            cursor_release();
        }
    }
    if (changed & MOUSE_BTN_RIGHT) {
        if (g_mouse.buttons & MOUSE_BTN_RIGHT)
            mouse_ui_dispatch_down(g_mouse.x, g_mouse.y, 2);
    }
}

/*
 * Poll for mouse data (non-interrupt fallback).
 * Call from main loop if IRQ12 is unreliable.
 */
void mouse_poll(void)
{
    uint8_t status = inb(PS2_STATUS_PORT);
    if ((status & PS2_STATUS_OBF) && (status & PS2_STATUS_AUX)) {
        uint8_t data = inb(PS2_DATA_PORT);

        switch (packet_index) {
        case 0:
            if (data & (1 << 3)) {
                packet[0] = data;
                packet_index = 1;
            }
            break;
        case 1:
            packet[1] = data;
            packet_index = 2;
            break;
        case 2:
            packet[2] = data;
            packet_index = 0;
            process_packet();
            break;
        }
    }
}

/* ── Accessors ── */

int mouse_get_x(void)
{
    return g_mouse.x;
}

int mouse_get_y(void)
{
    return g_mouse.y;
}

uint8_t mouse_get_buttons(void)
{
    return g_mouse.buttons;
}

const mouse_state_t *mouse_get_state(void)
{
    return &g_mouse;
}

void mouse_inject(int dx, int dy, uint8_t buttons)
{
    /* USB HID path -- still an input event. */
    idle_mark_active();

    /* USB HID boot mice already give us signed screen-coord deltas, so
     * skip the PS/2 sign-extension and Y inversion. */
    g_mouse.dx = dx;
    g_mouse.dy = dy;
    g_mouse.x += dx;
    g_mouse.y += dy;

    int w = (int)fb_width();
    int h = (int)fb_height();
    if (g_mouse.x < 0) g_mouse.x = 0;
    if (g_mouse.y < 0) g_mouse.y = 0;
    if (g_mouse.x >= w) g_mouse.x = w - 1;
    if (g_mouse.y >= h) g_mouse.y = h - 1;

    g_mouse.prev_buttons = g_mouse.buttons;
    g_mouse.buttons = buttons & 0x07;

    cursor_move(g_mouse.x, g_mouse.y);
    mouse_ui_dispatch_move(g_mouse.x, g_mouse.y);

    uint8_t changed = g_mouse.buttons ^ g_mouse.prev_buttons;
    if (changed & MOUSE_BTN_LEFT) {
        if (g_mouse.buttons & MOUSE_BTN_LEFT) {
            mouse_ui_dispatch_down(g_mouse.x, g_mouse.y, 1);
            cursor_press();
        } else {
            if (image_viewer_active())
                image_viewer_mouse_up(g_mouse.x, g_mouse.y, 1);
            mouse_ui_dispatch_up(g_mouse.x, g_mouse.y, 1);
            cursor_release();
        }
    }
    if (changed & MOUSE_BTN_RIGHT) {
        if (g_mouse.buttons & MOUSE_BTN_RIGHT)
            mouse_ui_dispatch_down(g_mouse.x, g_mouse.y, 2);
    }
}
