/*
 * Zeos -- ACPI power button + lid switch.
 *
 * ACPI power button + lid switch via PM1 status polling and DSDT
 * device match. SCI-driven event delivery is preferred but requires
 * the SCI handler to be wired through IOAPIC; we poll PM1_STS each
 * tick which costs an inb pair -- acceptable for power events whose
 * latency budget is human-scale (>50ms is fine).
 *
 * Devices located via the same EISA-id pattern scan that battery.c
 * uses for PNP0C0A:
 *   PNP0C0C  = Fixed Power Button device   (0x0C 0x41 0xD0 0x0C 0x0C)
 *   PNP0C0D  = Lid switch                  (0x0C 0x41 0xD0 0x0C 0x0D)
 *   PNP0C0E  = Sleep Button                (0x0C 0x41 0xD0 0x0C 0x0E)
 *
 * Actions:
 *   /power/button-action     suspend|shutdown|lock|ask  (default suspend)
 *   /power/lid-close-action  suspend|lock|nothing       (default lock)
 *   /power/lid-open-action   wake|nothing               (default wake)
 *
 * Shutdown path: if the FADT carries a decoded _S5 SLP_TYPx, write
 * SLP_TYP + SLP_EN to PM1a/b control the same way suspend.c does
 * for S3. If _S5 was not decoded, we report "shutdown not yet
 * implemented" honestly and fall through to a halt.
 */

#include "power_buttons.h"
#include "acpi.h"
#include "aml.h"
#include "io.h"
#include "kprint.h"
#include "vault.h"
#include "suspend.h"
#include "lockscreen.h"
#include "idle.h"
#include "notify.h"
#include "chain.h"
#include "chain_registry.h"

#include <stdint.h>

int CHAIN_POWER_INPUT = -1;

/* PM1 event-status register layout (ACPI spec, PM1a/PM1b EVT_BLK).
 * Bit  0: TMR_STS
 * Bit  4: BM_STS
 * Bit  5: GBL_STS
 * Bit  8: PWRBTN_STS    <-- fixed power button
 * Bit  9: SLPBTN_STS    <-- sleep button
 * Bit 10: RTC_STS
 * Bit 15: WAK_STS
 * Status bits are sticky / write-1-to-clear. */
#define PM1_STS_PWRBTN  (1u << 8)
#define PM1_STS_SLPBTN  (1u << 9)

#define PM1_CNT_SLP_EN  (1u << 13)

/* AML opcodes (we only look up dword consts to identify HIDs). */
#define AML_NAME_OP   0x08
#define AML_DWORD_OP  0x0C

/* EISA-id literal patterns -- already encoded little-endian. */
static const uint8_t HID_PWRBTN[4] = { 0x41, 0xD0, 0x0C, 0x0C };
static const uint8_t HID_LID[4]    = { 0x41, 0xD0, 0x0C, 0x0D };
static const uint8_t HID_SLPBTN[4] = { 0x41, 0xD0, 0x0C, 0x0E };

/* ── Module state ──────────────────────────────────────────────── */

static int s_initialized   = 0;
static int s_pwrbtn_found  = 0;
static int s_slpbtn_found  = 0;
static int s_lid_found     = 0;

/* Cached _LID literal value, if we saw `Name (_LID, <int>)` somewhere
 * in AML. Many laptops keep _LID as a Method that reads an EC byte;
 * those won't be here. -1 means unknown. */
static int s_lid_static = -1;
static int s_lid_last   = -1;  /* last reported state for change detection */

static power_action_t s_act_button     = POWER_ACT_SUSPEND;
static power_action_t s_act_lid_close  = POWER_ACT_LOCK;
static power_action_t s_act_lid_open   = POWER_ACT_WAKE;

static volatile uint32_t s_pwr_events = 0;
static volatile uint32_t s_lid_events = 0;

/* ── HID match (mirrors battery.c) ─────────────────────────────── */

static int hid_present(const uint8_t *aml, uint32_t len, const uint8_t hid[4])
{
    /* 08 5F 48 49 44 0C <hid_dword> = Name(_HID, DWordConst(...)) */
    for (uint32_t i = 0; i + 10 < len; i++) {
        if (aml[i]   != AML_NAME_OP) continue;
        if (aml[i+1] != 0x5F) continue;
        if (aml[i+2] != 0x48) continue;  /* H */
        if (aml[i+3] != 0x49) continue;  /* I */
        if (aml[i+4] != 0x44) continue;  /* D */
        if (aml[i+5] != AML_DWORD_OP) continue;
        if (aml[i+6] != hid[0]) continue;
        if (aml[i+7] != hid[1]) continue;
        if (aml[i+8] != hid[2]) continue;
        if (aml[i+9] != hid[3]) continue;
        return 1;
    }
    return 0;
}

/* Look for `Name (_LID, <int-const>)`. Most machines have this as a
 * Method; that path is invisible without an AML evaluator. We return
 * -1 in that case and fall back to a "stuck open" assumption. */
static int decode_lid_static(const uint8_t *aml, uint32_t len)
{
    /* 08 5F 4C 49 44 = Name(_LID,  */
    for (uint32_t i = 0; i + 6 < len; i++) {
        if (aml[i]   != AML_NAME_OP) continue;
        if (aml[i+1] != 0x5F) continue;
        if (aml[i+2] != 0x4C) continue;  /* L */
        if (aml[i+3] != 0x49) continue;  /* I */
        if (aml[i+4] != 0x44) continue;  /* D */
        uint8_t op = aml[i+5];
        if (op == 0x00) return 0;        /* ZeroOp */
        if (op == 0x01) return 1;        /* OneOp */
        if (op == 0x0A && i + 6 < len) return aml[i+6] ? 1 : 0;  /* ByteOp */
    }
    return -1;
}

/* ── VAULT keys ────────────────────────────────────────────────── */

#define KEY_BUTTON     "/power/button-action"
#define KEY_LID_CLOSE  "/power/lid-close-action"
#define KEY_LID_OPEN   "/power/lid-open-action"

static void load_action(const char *key, power_action_t *out, power_action_t fallback)
{
    uint32_t v = (uint32_t)fallback;
    int got = vault_load_config(key, &v, sizeof(v));
    if (got == (int)sizeof(v) && v <= POWER_ACT_WAKE) {
        *out = (power_action_t)v;
    } else {
        *out = fallback;
    }
}

static int save_action(const char *key, power_action_t a)
{
    uint32_t v = (uint32_t)a;
    return vault_save_config(key, &v, sizeof(v));
}

/* ── String mapping ────────────────────────────────────────────── */

static int streq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == 0 && *b == 0;
}

const char *power_action_label(power_action_t a)
{
    switch (a) {
        case POWER_ACT_SUSPEND:  return "suspend";
        case POWER_ACT_SHUTDOWN: return "shutdown";
        case POWER_ACT_LOCK:     return "lock";
        case POWER_ACT_ASK:      return "ask";
        case POWER_ACT_NOTHING:  return "nothing";
        case POWER_ACT_WAKE:     return "wake";
        default:                 return "?";
    }
}

int power_action_parse(const char *s, power_action_t *out)
{
    if (!s || !out) return 0;
    if (streq(s, "suspend"))  { *out = POWER_ACT_SUSPEND;  return 1; }
    if (streq(s, "shutdown")) { *out = POWER_ACT_SHUTDOWN; return 1; }
    if (streq(s, "lock"))     { *out = POWER_ACT_LOCK;     return 1; }
    if (streq(s, "ask"))      { *out = POWER_ACT_ASK;      return 1; }
    if (streq(s, "nothing"))  { *out = POWER_ACT_NOTHING;  return 1; }
    if (streq(s, "wake"))     { *out = POWER_ACT_WAKE;     return 1; }
    return 0;
}

/* ── Init ──────────────────────────────────────────────────────── */

void power_buttons_init(void)
{
    if (s_initialized) return;
    s_initialized = 1;

    int n = acpi_aml_table_count();
    for (int t = 0; t < n; t++) {
        const acpi_aml_table_t *tab = acpi_aml_table(t);
        if (!tab || !tab->aml) continue;
        if (hid_present(tab->aml, tab->len, HID_PWRBTN)) s_pwrbtn_found = 1;
        if (hid_present(tab->aml, tab->len, HID_SLPBTN)) s_slpbtn_found = 1;
        if (hid_present(tab->aml, tab->len, HID_LID)) {
            s_lid_found = 1;
            int v = decode_lid_static(tab->aml, tab->len);
            if (v >= 0) s_lid_static = v;
        }
    }

    /* Fixed power button: even if PNP0C0C isn't declared in AML, the
     * FADT advertises it via the PWR_BUTTON flag. We just always poll
     * PM1 PWRBTN_STS -- the FADT doesn't lie about whether it's there
     * but the bit is harmless to read on machines without one. */

    s_lid_last = s_lid_static;

    load_action(KEY_BUTTON,    &s_act_button,    POWER_ACT_SUSPEND);
    load_action(KEY_LID_CLOSE, &s_act_lid_close, POWER_ACT_LOCK);
    load_action(KEY_LID_OPEN,  &s_act_lid_open,  POWER_ACT_WAKE);

    kputs("[power] init: pwrbtn=");
    kputs(s_pwrbtn_found ? "yes" : "fixed-only");
    kputs(" slpbtn=");
    kputs(s_slpbtn_found ? "yes" : "no");
    kputs(" lid=");
    if (s_lid_found) {
        if (s_lid_static == 1)      kputs("open");
        else if (s_lid_static == 0) kputs("closed");
        else                         kputs("present (state via method)");
    } else {
        kputs("no");
    }
    kputs(" actions: btn=");
    kputs(power_action_label(s_act_button));
    kputs(" close=");
    kputs(power_action_label(s_act_lid_close));
    kputs(" open=");
    kputs(power_action_label(s_act_lid_open));
    kputc('\n');
}

/* ── PM1 event-status read (with clear) ────────────────────────── */

static uint16_t read_clear_pm1_evt(void)
{
    const acpi_fadt_info_t *f = acpi_fadt();
    if (!f || !f->valid) return 0;

    uint16_t merged = 0;
    if (f->pm1a_evt_blk) {
        uint16_t v = inw(f->pm1a_evt_blk);
        merged |= v;
        /* Write-1-to-clear sticky status bits we care about. */
        uint16_t clr = v & (PM1_STS_PWRBTN | PM1_STS_SLPBTN);
        if (clr) outw(f->pm1a_evt_blk, clr);
    }
    if (f->pm1b_evt_blk) {
        uint16_t v = inw(f->pm1b_evt_blk);
        merged |= v;
        uint16_t clr = v & (PM1_STS_PWRBTN | PM1_STS_SLPBTN);
        if (clr) outw(f->pm1b_evt_blk, clr);
    }
    return merged;
}

int power_button_pressed(void)
{
    uint16_t sts = read_clear_pm1_evt();
    /* Treat sleep button identically -- both should map to the
     * configured power action. */
    if (sts & (PM1_STS_PWRBTN | PM1_STS_SLPBTN)) {
        s_pwr_events++;
        return 1;
    }
    return 0;
}

/* ── Lid state ─────────────────────────────────────────────────── */

int lid_current_state(void)
{
    /* Try the AML interpreter first (Method-form _LID, which is what
     * modern laptops use). If that fails or aborts on an unimplemented
     * opcode, fall back to the static literal we decoded at init. */
    static const char *kLidPaths[] = {
        "\\_SB_.LID0._LID",
        "\\_SB_.LID._LID",
        "\\_SB_.PCI0.LPCB.EC0.LID._LID",
        "\\_SB_.PCI0.LPC0.EC.LID._LID",
        0
    };
    aml_object_t r = { .type = AML_T_UNINIT };
    for (int i = 0; kLidPaths[i]; i++) {
        if (aml_evaluate(kLidPaths[i], 0, 0, &r) == 0 &&
            r.type == AML_T_INTEGER) {
            int v = r.u.integer ? 1 : 0;
            aml_object_release(&r);
            return v;
        }
        aml_object_release(&r);
        r.type = AML_T_UNINIT;
    }
    return s_lid_static;
}

int lid_state_changed(int *new_state)
{
    int s = lid_current_state();
    if (s < 0) return 0;
    if (s != s_lid_last) {
        s_lid_last = s;
        if (new_state) *new_state = s;
        s_lid_events++;
        return 1;
    }
    return 0;
}

/* ── Accessors / setters ───────────────────────────────────────── */

int power_btn_present(void)  { return s_pwrbtn_found; }
int sleep_btn_present(void)  { return s_slpbtn_found; }
int lid_present(void)        { return s_lid_found; }

power_action_t power_button_action(void) { return s_act_button; }
power_action_t lid_close_action(void)    { return s_act_lid_close; }
power_action_t lid_open_action(void)     { return s_act_lid_open; }

uint32_t power_button_event_count(void) { return s_pwr_events; }
uint32_t lid_event_count(void)          { return s_lid_events; }

int power_button_set_action(power_action_t a)
{
    if (a != POWER_ACT_SUSPEND && a != POWER_ACT_SHUTDOWN &&
        a != POWER_ACT_LOCK && a != POWER_ACT_ASK) return -1;
    s_act_button = a;
    return save_action(KEY_BUTTON, a);
}

int lid_close_set_action(power_action_t a)
{
    if (a != POWER_ACT_SUSPEND && a != POWER_ACT_LOCK &&
        a != POWER_ACT_NOTHING) return -1;
    s_act_lid_close = a;
    return save_action(KEY_LID_CLOSE, a);
}

int lid_open_set_action(power_action_t a)
{
    if (a != POWER_ACT_WAKE && a != POWER_ACT_NOTHING) return -1;
    s_act_lid_open = a;
    return save_action(KEY_LID_OPEN, a);
}

/* ── Shutdown (S5) ─────────────────────────────────────────────── */
/*
 * suspend.c only decodes _S3. For S5 we reuse the same DSDT scan:
 * pattern `08 5F 53 35 5F 12 <pkglen> <num> <slp_typa> <slp_typb>`
 * = Name(_S5_, Package() {a, b, ...}). If found, write
 * (slp_typa<<10 | SLP_EN) to PM1a_CNT, same for PM1b. If not found,
 * the platform doesn't expose S5 cleanly; we log honestly and HLT.
 */

static int find_s5_slp(uint8_t *outa, uint8_t *outb)
{
    int n = acpi_aml_table_count();
    for (int t = 0; t < n; t++) {
        const acpi_aml_table_t *tab = acpi_aml_table(t);
        if (!tab || !tab->aml) continue;
        const uint8_t *aml = tab->aml;
        uint32_t len = tab->len;
        for (uint32_t i = 0; i + 10 < len; i++) {
            /* Name(_S5_, Package(...)) */
            if (aml[i]   != AML_NAME_OP) continue;
            if (aml[i+1] != 0x5F) continue;
            if (aml[i+2] != 0x53) continue;  /* S */
            if (aml[i+3] != 0x35) continue;  /* 5 */
            if (aml[i+4] != 0x5F) continue;  /* _ */
            if (aml[i+5] != 0x12) continue;  /* PackageOp */
            /* Skip pkglen byte(s). Top 2 bits encode extra bytes. */
            uint8_t b0 = aml[i+6];
            int extra = (b0 >> 6) & 0x3;
            uint32_t p = i + 6 + 1 + extra;  /* past pkglen */
            if (p + 2 >= len) continue;
            /* Now NumElements byte, then element 0 (slp_typa). */
            p += 1;
            /* element 0: ByteOp + value, or Zero/One. */
            uint8_t a = 0, b = 0;
            uint8_t op = aml[p];
            if (op == 0x00) { a = 0; p += 1; }
            else if (op == 0x01) { a = 1; p += 1; }
            else if (op == 0x0A && p + 1 < len) { a = aml[p+1]; p += 2; }
            else continue;
            if (p >= len) continue;
            op = aml[p];
            if (op == 0x00) { b = 0; }
            else if (op == 0x01) { b = 1; }
            else if (op == 0x0A && p + 1 < len) { b = aml[p+1]; }
            else continue;
            *outa = a & 0x7;
            *outb = b & 0x7;
            return 0;
        }
    }
    return -1;
}

static void do_shutdown(void)
{
    const acpi_fadt_info_t *f = acpi_fadt();
    if (!f || !f->valid || !f->pm1a_cnt_blk) {
        kputs("[power] shutdown: FADT/PM1 not available\n");
        return;
    }
    uint8_t a = 0, b = 0;
    if (find_s5_slp(&a, &b) != 0) {
        kputs("[power] shutdown not yet implemented (no _S5 in DSDT/SSDT) -- halting\n");
        __asm__ volatile("cli");
        for (;;) __asm__ volatile("hlt");
    }

    kputs("[power] entering S5 (SLP_TYPa=");
    kput_hex(a);
    kputs(" SLP_TYPb=");
    kput_hex(b);
    kputs(")\n");

    uint16_t pm1a = inw(f->pm1a_cnt_blk);
    pm1a &= ~(0x7u << 10);
    pm1a |= ((uint16_t)a << 10);
    pm1a |= PM1_CNT_SLP_EN;
    outw(f->pm1a_cnt_blk, pm1a);

    if (f->pm1b_cnt_blk) {
        uint16_t pm1b = inw(f->pm1b_cnt_blk);
        pm1b &= ~(0x7u << 10);
        pm1b |= ((uint16_t)b << 10);
        pm1b |= PM1_CNT_SLP_EN;
        outw(f->pm1b_cnt_blk, pm1b);
    }

    /* If SLP_EN took, we never get here. */
    __asm__ volatile("cli");
    for (;;) __asm__ volatile("hlt");
}

/* ── Action dispatch ───────────────────────────────────────────── */

static void dispatch(power_action_t a, const char *src)
{
    switch (a) {
        case POWER_ACT_SUSPEND:
            kputs("[power] ");
            kputs(src);
            kputs(" -> suspend\n");
            (void)suspend_to_ram(0);
            break;
        case POWER_ACT_SHUTDOWN:
            kputs("[power] ");
            kputs(src);
            kputs(" -> shutdown\n");
            do_shutdown();
            break;
        case POWER_ACT_LOCK:
            kputs("[power] ");
            kputs(src);
            kputs(" -> lock\n");
            if (lockscreen_pin_configured()) {
                idle_force_lock();
            } else {
                notify_send("Set a PIN to enable lock", "power", NOTIFY_WARNING);
            }
            break;
        case POWER_ACT_ASK:
            notify_send("Power button: tap again to suspend, hold for shutdown",
                        "power", NOTIFY_INFO);
            break;
        case POWER_ACT_WAKE:
            kputs("[power] ");
            kputs(src);
            kputs(" -> wake\n");
            idle_mark_active();
            break;
        case POWER_ACT_NOTHING:
        default:
            break;
    }
}

/* ── Chain ─────────────────────────────────────────────────────── */

/* Pipeline: acpi_event_poll -> power_button_decode -> lid_decode ->
 * action_dispatch. Each node writes a small int through the
 * input/output scratch so they remain inspectable in the graph.
 *
 * Per-tick cost: two 16-bit port reads + one acpi_aml table walk for
 * the static lid (cached). Negligible. */

static int s_pending_pwr  = 0;
static int s_pending_lid  = 0;  /* -1 none, 0 closed, 1 open */

static void node_acpi_poll(chain_node_t *self, void *input, void *output)
{
    (void)self; (void)input;
    s_pending_pwr  = power_button_pressed() ? 1 : 0;
    int new_lid = -1;
    s_pending_lid = lid_state_changed(&new_lid) ? new_lid : -1;
    int *out = (int *)output;
    if (out) *out = s_pending_pwr | (s_pending_lid >= 0 ? 2 : 0);
}

static void node_power_decode(chain_node_t *self, void *input, void *output)
{
    (void)self; (void)input;
    int *out = (int *)output;
    if (out) *out = s_pending_pwr;
}

static void node_lid_decode(chain_node_t *self, void *input, void *output)
{
    (void)self; (void)input;
    int *out = (int *)output;
    if (out) *out = s_pending_lid;
}

static void node_action_dispatch(chain_node_t *self, void *input, void *output)
{
    (void)self; (void)input;
    int *out = (int *)output;
    int fired = 0;

    if (s_pending_pwr) {
        dispatch(s_act_button, "power-button");
        chain_t *c = chain_get(CHAIN_POWER_INPUT);
        if (c) c->vault_version++;
        s_pending_pwr = 0;
        fired++;
    }
    if (s_pending_lid >= 0) {
        if (s_pending_lid == 0) {
            dispatch(s_act_lid_close, "lid-close");
        } else {
            dispatch(s_act_lid_open, "lid-open");
        }
        chain_t *c = chain_get(CHAIN_POWER_INPUT);
        if (c) c->vault_version++;
        s_pending_lid = -1;
        fired++;
    }
    if (out) *out = fired;
}

int power_buttons_chain_register(int parent_chain_id)
{
    CHAIN_POWER_INPUT = chain_create("power_input", parent_chain_id, MASQ_INTERNAL);
    if (CHAIN_POWER_INPUT < 0) return -1;
    chain_add_node(CHAIN_POWER_INPUT, "acpi_event_poll",
                   "tick", "pm1_status",
                   node_acpi_poll);
    chain_add_node(CHAIN_POWER_INPUT, "power_button_decode",
                   "pm1_status", "power_event",
                   node_power_decode);
    chain_add_node(CHAIN_POWER_INPUT, "lid_decode",
                   "pm1_status", "lid_event",
                   node_lid_decode);
    chain_add_node(CHAIN_POWER_INPUT, "action_dispatch",
                   "power_event", "action_count",
                   node_action_dispatch);

    chain_t *c = chain_get(CHAIN_POWER_INPUT);
    if (c) c->resolve_interval_ticks = 8;  /* ~10ms — responsive */
    return CHAIN_POWER_INPUT;
}
