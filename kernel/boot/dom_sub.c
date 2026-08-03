/*
 * Zeos — Dom/Sub cooperative multi-chip PCIe compute fabric.
 * BUILD_MAP Q.1–Q.4. Spec: specs/DOM_SUB_CHIPS.md.
 *
 * Built entirely on infra Zeos already has: gpu_goya.c (chip enumeration +
 * per-card records) for boot-seed + identify, and hotplug.c's public event ring
 * (hotplug_event_copy) for live attach/detach — no edit to hotplug.c needed.
 */
#include "dom_sub.h"
#include "gpu_goya.h"
#include "hotplug.h"
#include "kprint.h"
#include "timer.h"

/* ── Q.1: chip-class allowlist (append-only per BIBLE G1). ──────────── */
static const chip_class_t CHIP_CLASSES[] = {
    { 0x1DA3, 0x0001, "goya" },   /* confirmed real hardware, fleet-tested */
    /* future entries appended here, never removed */
};
#define NUM_CHIP_CLASSES  (int)(sizeof(CHIP_CLASSES) / sizeof(CHIP_CLASSES[0]))

const char *dom_sub_chip_label(uint16_t vendor_id, uint16_t device_id)
{
    for (int i = 0; i < NUM_CHIP_CLASSES; i++)
        if (CHIP_CLASSES[i].vendor_id == vendor_id &&
            CHIP_CLASSES[i].device_id == device_id)
            return CHIP_CLASSES[i].label;
    return 0;
}

int dom_sub_is_cooperative_chip(uint16_t vendor_id, uint16_t device_id)
{
    return dom_sub_chip_label(vendor_id, device_id) != 0;
}

/* ── Cohort state ──────────────────────────────────────────────────── */
static cohort_member_t s_members[DOM_SUB_MAX_CHIPS];
static int      s_dom = -1;          /* current Dom member index */
static int      s_chal = -1;         /* challenger tracked for hysteresis */
static int      s_chal_streak = 0;   /* consecutive elections chal has led */
static uint64_t s_evt_cursor = 0;    /* hotplug drain: last processed tsc */

int dom_sub_count(void)
{
    int n = 0;
    for (int i = 0; i < DOM_SUB_MAX_CHIPS; i++)
        if (s_members[i].in_use) n++;
    return n;
}

const cohort_member_t *dom_sub_member(int idx)
{
    if (idx < 0 || idx >= DOM_SUB_MAX_CHIPS || !s_members[idx].in_use) return 0;
    return &s_members[idx];
}

int dom_sub_dom_index(void) { return s_dom; }

/* ── Q.3: identify ─────────────────────────────────────────────────── */
/* Fill a member's identity. Real fw_version + DRAM come from the bound Goya
 * record (gpu_goya_device); dram_mb is derived from the DDR aperture (BAR4)
 * length. bench_score starts 0 (lazily filled by the first real THINK job —
 * Q.5/Q.7). Synthetic members (goya_idx<0) keep their injected identity. */
static void dom_sub_identify(cohort_member_t *m)
{
    if (m->goya_idx < 0) return;   /* synthetic: identity was injected */
    m->id.fw_version = 0;
    m->id.dram_mb = 0;
    m->id.thermal_margin = 0;
    m->id.bench_score = 0;
    const gpu_goya_device_t *d = gpu_goya_device(m->goya_idx);
    if (d) {
        m->id.fw_version = d->fw_version;
        m->id.dram_mb = (uint32_t)(d->bar4_len >> 20);   /* DDR aperture MB */
    }
}

/* ── Q.4: election ─────────────────────────────────────────────────── */
static uint32_t member_score(const cohort_member_t *m)
{
    return m->id.bench_score * 1000u + m->id.dram_mb;
}

/* Lowest PCI bus/dev/func wins the tiebreak. */
static int pci_less(const cohort_member_t *a, const cohort_member_t *b)
{
    if (a->bus != b->bus) return a->bus < b->bus;
    if (a->dev != b->dev) return a->dev < b->dev;
    return a->func < b->func;
}

/* Highest score, PCI-address tiebreak. -1 if the cohort is empty. */
static int best_member(void)
{
    int best = -1;
    for (int i = 0; i < DOM_SUB_MAX_CHIPS; i++) {
        if (!s_members[i].in_use) continue;
        if (best < 0) { best = i; continue; }
        uint32_t sb = member_score(&s_members[best]);
        uint32_t si = member_score(&s_members[i]);
        if (si > sb || (si == sb && pci_less(&s_members[i], &s_members[best])))
            best = i;
    }
    return best;
}

static void assign_roles(void)
{
    for (int i = 0; i < DOM_SUB_MAX_CHIPS; i++) {
        if (!s_members[i].in_use) { s_members[i].role = DOM_SUB_ROLE_NONE; continue; }
        s_members[i].role = (i == s_dom) ? DOM_SUB_ROLE_DOM : DOM_SUB_ROLE_SUB;
    }
}

/* State-transition log (BIBLE G2: timestamp + reason, not a silent flip). */
static void set_dom(int idx, const char *reason)
{
    int prev = s_dom;
    s_dom = idx;
    kputs("  Dom/Sub: ELECT tsc=");
    kput_hex(timer_read_tsc());
    kputs(" dom=");
    if (idx >= 0) {
        kputs(s_members[idx].label ? s_members[idx].label : "chip");
        kputs("@"); kput_dec(s_members[idx].bus);
        kputc(':'); kput_dec(s_members[idx].dev);
        kputc('.'); kput_dec(s_members[idx].func);
        kputs(" score="); kput_dec(member_score(&s_members[idx]));
    } else {
        kputs("(none)");
    }
    kputs(" was="); kput_dec(prev);
    kputs(" reason="); kputs(reason); kputc('\n');
}

int dom_sub_elect(void)
{
    int best = best_member();
    if (best < 0) {                 /* empty cohort */
        if (s_dom != -1) set_dom(-1, "cohort empty");
        s_chal = -1; s_chal_streak = 0;
        assign_roles();
        return -1;
    }
    /* No incumbent, or the incumbent Dom departed → immediate (no hysteresis). */
    if (s_dom < 0 || !s_members[s_dom].in_use) {
        set_dom(best, s_dom < 0 ? "no incumbent" : "dom departed");
        s_chal = -1; s_chal_streak = 0;
        assign_roles();
        return s_dom;
    }
    if (best == s_dom) {            /* incumbent still the best */
        s_chal = -1; s_chal_streak = 0;
        assign_roles();
        return s_dom;
    }
    /* A challenger outscores the Dom → require a sustained >=PCT margin. */
    uint32_t dom_s  = member_score(&s_members[s_dom]);
    uint32_t chal_s = member_score(&s_members[best]);
    int margin_ok = (uint64_t)chal_s * 100u >=
                    (uint64_t)dom_s * (100u + DOM_SUB_HYSTERESIS_PCT);
    if (margin_ok && best == s_chal) {
        s_chal_streak++;
    } else if (margin_ok) {
        s_chal = best; s_chal_streak = 1;
    } else {
        s_chal = -1; s_chal_streak = 0;   /* too close — no flap */
    }
    if (s_chal_streak >= DOM_SUB_HYSTERESIS_N) {
        set_dom(best, "challenger beat dom by sustained margin");
        s_chal = -1; s_chal_streak = 0;
    }
    assign_roles();
    return s_dom;
}

/* ── Q.2: membership ───────────────────────────────────────────────── */
static int find_member(uint8_t bus, uint8_t dev, uint8_t func)
{
    for (int i = 0; i < DOM_SUB_MAX_CHIPS; i++)
        if (s_members[i].in_use && s_members[i].bus == bus &&
            s_members[i].dev == dev && s_members[i].func == func)
            return i;
    return -1;
}

int dom_sub_on_attach(uint8_t bus, uint8_t dev, uint8_t func,
                      uint16_t vendor, uint16_t device, int goya_idx)
{
    if (!dom_sub_is_cooperative_chip(vendor, device)) return -1;
    int ex = find_member(bus, dev, func);
    if (ex >= 0) return ex;         /* already in the cohort */

    int slot = -1;
    for (int i = 0; i < DOM_SUB_MAX_CHIPS; i++)
        if (!s_members[i].in_use) { slot = i; break; }
    if (slot < 0) {
        kputs("  Dom/Sub: cohort full, cannot add chip\n");
        return -1;
    }
    cohort_member_t *m = &s_members[slot];
    m->in_use = 1;
    m->bus = bus; m->dev = dev; m->func = func;
    m->vendor_id = vendor; m->device_id = device;
    m->label = dom_sub_chip_label(vendor, device);
    m->goya_idx = goya_idx;
    m->id.fw_version = 0; m->id.dram_mb = 0;
    m->id.thermal_margin = 0; m->id.bench_score = 0;
    m->role = DOM_SUB_ROLE_SUB;
    dom_sub_identify(m);

    kputs("  Dom/Sub: + chip "); kputs(m->label ? m->label : "?");
    kputs(" @"); kput_dec(bus); kputc(':'); kput_dec(dev); kputc('.'); kput_dec(func);
    kputs(" fw="); kput_hex(m->id.fw_version);
    kputs(" dram="); kput_dec(m->id.dram_mb); kputs("MB\n");

    dom_sub_elect();
    return slot;
}

void dom_sub_on_detach(uint8_t bus, uint8_t dev, uint8_t func)
{
    int idx = find_member(bus, dev, func);
    if (idx < 0) return;
    int was_dom = (idx == s_dom);
    kputs("  Dom/Sub: - chip @"); kput_dec(bus); kputc(':'); kput_dec(dev);
    kputc('.'); kput_dec(func); kputs(was_dom ? " (was Dom)\n" : " (was Sub)\n");
    s_members[idx].in_use = 0;
    s_members[idx].role = DOM_SUB_ROLE_NONE;
    if (was_dom) { s_dom = -1; s_chal = -1; s_chal_streak = 0; }  /* immediate */
    dom_sub_elect();
}

/* ── Q.2: boot-seed + live drain ───────────────────────────────────── */
void dom_sub_init(void)
{
    int n = gpu_goya_device_count();
    for (int i = 0; i < n; i++) {
        const gpu_goya_device_t *d = gpu_goya_device(i);
        if (!d) continue;
        if (!dom_sub_is_cooperative_chip(d->pci_vendor, d->pci_device)) continue;
        dom_sub_on_attach(d->pci_bus, d->pci_dev, d->pci_func,
                          d->pci_vendor, d->pci_device, i);
    }
    /* Only react to hotplug events emitted after boot-seed. */
    s_evt_cursor = timer_read_tsc();
    kputs("  Dom/Sub: cohort seeded, "); kput_dec(dom_sub_count());
    kputs(" cooperative chip(s)\n");
}

/* Match a live PCI address back to a bound Goya record (for identify). */
static int goya_idx_for(uint8_t bus, uint8_t dev, uint8_t func)
{
    int n = gpu_goya_device_count();
    for (int i = 0; i < n; i++) {
        const gpu_goya_device_t *d = gpu_goya_device(i);
        if (d && d->pci_bus == bus && d->pci_dev == dev && d->pci_func == func)
            return i;
    }
    return -1;
}

void dom_sub_poll(void)
{
    hotplug_event_t buf[32];
    int got = hotplug_event_copy(buf, 32);
    uint64_t newest = s_evt_cursor;
    for (int i = 0; i < got; i++) {
        hotplug_event_t *e = &buf[i];
        if (e->tsc <= s_evt_cursor) continue;    /* already processed */
        if (e->tsc > newest) newest = e->tsc;
        uint8_t bus  = (uint8_t)e->bus;
        uint8_t dev  = (uint8_t)((e->devfn >> 3) & 0x1F);
        uint8_t func = (uint8_t)(e->devfn & 0x07);
        if (e->kind == HOTPLUG_EVT_PCI_ATTACH &&
            dom_sub_is_cooperative_chip(e->vendor_id, e->product_id)) {
            dom_sub_on_attach(bus, dev, func, e->vendor_id, e->product_id,
                              goya_idx_for(bus, dev, func));
        } else if (e->kind == HOTPLUG_EVT_PCI_DETACH) {
            dom_sub_on_detach(bus, dev, func);
        }
    }
    s_evt_cursor = newest;
}

/* ── Introspection / verification ──────────────────────────────────── */
void dom_sub_dump(void)
{
    int n = dom_sub_count();
    kputs("  Dom/Sub cohort ("); kput_dec(n); kputs(" chip(s), dom=");
    kput_dec(s_dom); kputs("):\n");
    for (int i = 0; i < DOM_SUB_MAX_CHIPS; i++) {
        cohort_member_t *m = &s_members[i];
        if (!m->in_use) continue;
        kputs("    ["); kput_dec(i); kputs("] ");
        kputs(m->role == DOM_SUB_ROLE_DOM ? "DOM " : "SUB ");
        kputs(m->label ? m->label : "chip");
        kputs(" @"); kput_dec(m->bus); kputc(':'); kput_dec(m->dev);
        kputc('.'); kput_dec(m->func);
        kputs(" dram="); kput_dec(m->id.dram_mb); kputs("MB");
        kputs(" bench="); kput_dec(m->id.bench_score);
        kputs(" score="); kput_dec(member_score(m)); kputc('\n');
    }
}

void dom_sub_reset(void)
{
    for (int i = 0; i < DOM_SUB_MAX_CHIPS; i++) {
        s_members[i].in_use = 0;
        s_members[i].role = DOM_SUB_ROLE_NONE;
    }
    s_dom = -1; s_chal = -1; s_chal_streak = 0;
}

int dom_sub_inject_synthetic(uint8_t bus, uint8_t dev, uint8_t func,
                             uint32_t dram_mb, uint32_t bench_score)
{
    int slot = dom_sub_on_attach(bus, dev, func, 0x1DA3, 0x0001, -1);
    if (slot < 0) return -1;
    s_members[slot].id.dram_mb = dram_mb;
    s_members[slot].id.bench_score = bench_score;
    dom_sub_elect();               /* re-score with the injected identity */
    return slot;
}

/* Q.1 + Q.4 verification with no hardware: exercise the chip-class table + the
 * election algorithm (scoring, PCI tiebreak, hysteresis, dom-detach). */
static int expect(const char *what, int got, int want)
{
    int ok = (got == want);
    kputs(ok ? "    PASS " : "    FAIL ");
    kputs(what); kputs(" got="); kput_dec(got); kputs(" want="); kput_dec(want);
    kputc('\n');
    return ok;
}

int dom_sub_selftest(void)
{
    int ok = 1;
    kputs("  Dom/Sub selftest:\n");

    /* Q.1: chip-class table. */
    ok &= expect("goya recognized", dom_sub_is_cooperative_chip(0x1DA3, 0x0001), 1);
    ok &= expect("random rejected", dom_sub_is_cooperative_chip(0x8086, 0x1234), 0);

    /* Q.4a: empty cohort → no Dom. */
    dom_sub_reset();
    ok &= expect("empty elects -1", dom_sub_elect(), -1);

    /* Q.4b: first chip becomes Dom immediately. */
    dom_sub_inject_synthetic(1, 0, 0, /*dram*/8192, /*bench*/0);      /* slot 0 */
    ok &= expect("first chip is Dom", dom_sub_dom_index(), 0);

    /* Q.4c: a much better chip (2x score) is a challenger — hysteresis holds
     * the incumbent for N-1 elections, then unseats on the Nth. */
    dom_sub_inject_synthetic(2, 0, 0, /*dram*/16384, /*bench*/0);     /* slot 1 */
    ok &= expect("Dom held after 1 (hysteresis)", dom_sub_dom_index(), 0);
    dom_sub_elect();                                                  /* streak 2 */
    ok &= expect("Dom held after 2 (hysteresis)", dom_sub_dom_index(), 0);
    dom_sub_elect();                                                  /* streak 3 */
    ok &= expect("challenger wins on Nth", dom_sub_dom_index(), 1);

    /* Q.4d: equal-score challenger never unseats (margin not met). */
    dom_sub_reset();
    dom_sub_inject_synthetic(5, 0, 0, 4096, 0);                       /* slot 0 = Dom */
    dom_sub_inject_synthetic(3, 0, 0, 4096, 0);                       /* slot 1, lower PCI */
    dom_sub_elect(); dom_sub_elect(); dom_sub_elect();
    ok &= expect("equal score keeps incumbent", dom_sub_dom_index(), 0);

    /* Q.4e: Dom-detach → immediate re-election to the surviving chip. */
    dom_sub_reset();
    dom_sub_inject_synthetic(1, 0, 0, 8192, 0);                       /* slot 0 = Dom */
    dom_sub_inject_synthetic(2, 0, 0, 4096, 0);                       /* slot 1 = Sub */
    ok &= expect("higher-score chip is Dom", dom_sub_dom_index(), 0);
    dom_sub_on_detach(1, 0, 0);                                       /* unplug Dom */
    ok &= expect("survivor promoted immediately", dom_sub_dom_index(), 1);

    dom_sub_reset();
    kputs(ok ? "  Dom/Sub selftest: PASS\n" : "  Dom/Sub selftest: FAIL\n");
    return ok;
}
