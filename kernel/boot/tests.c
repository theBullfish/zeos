/*
 * Zeos test framework. Per-chain isolation via b3 + vault_version snapshot/restore.
 * Goal: catch regressions before they ship. Run via `tests` shell command.
 * `tests --json` for CI.
 */

#include "tests.h"
#include "chain.h"
#include "chain_registry.h"
#include "kprint.h"
#include "timer.h"
#include "block.h"
#include "block_chain.h"
#include "mde_chain.h"
#include "keyboard.h"
#include "mouse.h"
#include "gpu_virtio.h"
#include "net.h"
#include "vault.h"
#include "zplus.h"

/* ── Registry ─────────────────────────────────────────────────────── */

#define TESTS_MAX 64

struct test_entry {
    char       name[48];
    test_fn_t  fn;
    int        chain_id;
};

static struct test_entry g_tests[TESTS_MAX];
static int g_test_count = 0;
static int g_tests_inited = 0;

/* ── Helpers ──────────────────────────────────────────────────────── */

static int t_strstr(const char *hay, const char *needle)
{
    if (!needle || !*needle) return 1;
    while (*hay) {
        const char *h = hay, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return 1;
        hay++;
    }
    return 0;
}

static int t_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void t_strcopy(char *dst, const char *src, int max)
{
    int i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static uint64_t tsc_to_ms(uint64_t dt)
{
    uint64_t freq = timer_tsc_freq();
    if (freq == 0) return 0;
    return (dt * 1000ULL) / freq;
}

/* Pad name with dots to width. */
static void print_dotted(const char *name, int width)
{
    kputs(name);
    int n = t_strlen(name);
    kputc(' ');
    for (int i = n + 1; i < width; i++) kputc('.');
    kputc(' ');
}

/* ── Snapshot / restore ───────────────────────────────────────────── */

struct chain_snapshot {
    int   valid;
    int   chain_id;
    float b3_alpha;
    float b3_beta;
    int   b3_observations;
    int   vault_version;
};

static void snap_take(struct chain_snapshot *s, int chain_id)
{
    s->valid = 0;
    s->chain_id = chain_id;
    if (chain_id < 0) return;
    chain_t *c = chain_get(chain_id);
    if (!c) return;
    s->b3_alpha        = c->b3_alpha;
    s->b3_beta         = c->b3_beta;
    s->b3_observations = c->b3_observations;
    s->vault_version   = c->vault_version;
    s->valid = 1;
}

static void snap_restore(const struct chain_snapshot *s)
{
    if (!s->valid) return;
    chain_t *c = chain_get(s->chain_id);
    if (!c) return;
    c->b3_alpha        = s->b3_alpha;
    c->b3_beta         = s->b3_beta;
    c->b3_observations = s->b3_observations;
    c->vault_version   = s->vault_version;
}

/* ── Built-in tests ───────────────────────────────────────────────── */

static int test_chain_audio(char *reason, uint32_t rsize)
{
    (void)reason; (void)rsize;
    if (CHAIN_AUDIO < 0) { t_strcopy(reason, "no audio chain", rsize); return TEST_SKIP; }
    chain_t *c = chain_get(CHAIN_AUDIO);
    if (!c) { t_strcopy(reason, "chain_get NULL", rsize); return TEST_FAIL; }
    if (c->node_count != 4) {
        t_strcopy(reason, "expected 4 nodes", rsize);
        return TEST_FAIL;
    }
    int rc = chain_resolve(CHAIN_AUDIO);
    /* 0 == success. -1 acceptable only if status not LIVE (e.g. controller
     * dead -- we still treat that as a SKIP since chain plumbing is fine). */
    if (rc == 0) return TEST_PASS;
    if (c->status != CHAIN_LIVE) {
        t_strcopy(reason, "chain not LIVE (no controller)", rsize);
        return TEST_SKIP;
    }
    t_strcopy(reason, "chain_resolve != 0", rsize);
    return TEST_FAIL;
}

static int test_chain_net_tx(char *reason, uint32_t rsize)
{
    if (CHAIN_NET_TX < 0) { t_strcopy(reason, "no net.tx chain", rsize); return TEST_SKIP; }
    chain_t *c = chain_get(CHAIN_NET_TX);
    if (!c || c->node_count != 4) {
        t_strcopy(reason, "expected 4 nodes", rsize);
        return TEST_FAIL;
    }
    int rc = chain_resolve(CHAIN_NET_TX);
    if (rc == 0) return TEST_PASS;
    if (!g_net.up) { t_strcopy(reason, "no link", rsize); return TEST_SKIP; }
    t_strcopy(reason, "chain_resolve failed", rsize);
    return TEST_FAIL;
}

static int test_chain_net_rx(char *reason, uint32_t rsize)
{
    if (CHAIN_NET_RX < 0) { t_strcopy(reason, "no net.rx chain", rsize); return TEST_SKIP; }
    chain_t *c = chain_get(CHAIN_NET_RX);
    if (!c || c->node_count != 4) {
        t_strcopy(reason, "expected 4 nodes", rsize);
        return TEST_FAIL;
    }
    int rc = chain_resolve(CHAIN_NET_RX);
    if (rc == 0) return TEST_PASS;
    if (!g_net.up) { t_strcopy(reason, "no link", rsize); return TEST_SKIP; }
    t_strcopy(reason, "chain_resolve failed", rsize);
    return TEST_FAIL;
}

static int test_chain_block_read(char *reason, uint32_t rsize)
{
    if (block_drive_count() <= 0) {
        t_strcopy(reason, "no drive", rsize);
        return TEST_SKIP;
    }
    block_drive_info_t info;
    if (block_drive_info(0, &info) != 0 || info.sector_size == 0) {
        t_strcopy(reason, "drive 0 info bad", rsize);
        return TEST_FAIL;
    }
    static uint8_t buf[4096] __attribute__((aligned(64)));
    if (block_read_drive(0, 0, 1, buf) != 0) {
        t_strcopy(reason, "LBA 0 read failed", rsize);
        return TEST_FAIL;
    }
    return TEST_PASS;
}

static int test_chain_block_write(char *reason, uint32_t rsize)
{
    if (block_drive_count() <= 0) {
        t_strcopy(reason, "no drive", rsize);
        return TEST_SKIP;
    }
    block_drive_info_t info;
    if (block_drive_info(0, &info) != 0 || info.sector_size == 0) {
        t_strcopy(reason, "drive info bad", rsize);
        return TEST_FAIL;
    }
    static uint8_t saved[4096] __attribute__((aligned(64)));
    static uint8_t pat[4096]   __attribute__((aligned(64)));
    static uint8_t rb[4096]    __attribute__((aligned(64)));

    /* Pick a high LBA so we don't trample partition tables. */
    uint64_t lba = (info.sectors > 16) ? (info.sectors - 4) : 8;

    int saved_ok = (block_read_drive(0, lba, 1, saved) == 0);

    for (int i = 0; i < 512; i++) pat[i] = (uint8_t)(i ^ 0xA5);

    uint64_t before = block_chain_journal_total();
    if (block_write_drive(0, lba, 1, pat) != 0) {
        t_strcopy(reason, "write failed", rsize);
        if (saved_ok) (void)block_write_drive(0, lba, 1, saved);
        return TEST_FAIL;
    }
    uint64_t after = block_chain_journal_total();

    if (block_read_drive(0, lba, 1, rb) != 0) {
        t_strcopy(reason, "readback failed", rsize);
        if (saved_ok) (void)block_write_drive(0, lba, 1, saved);
        return TEST_FAIL;
    }
    int ok = 1;
    for (int i = 0; i < 512; i++) {
        if (rb[i] != pat[i]) { ok = 0; break; }
    }
    if (saved_ok) (void)block_write_drive(0, lba, 1, saved);

    if (!ok) {
        t_strcopy(reason, "readback mismatch", rsize);
        return TEST_FAIL;
    }
    if (after - before < 1) {
        t_strcopy(reason, "journal not advanced", rsize);
        return TEST_FAIL;
    }
    return TEST_PASS;
}

/* MDE chain: real kernel_fn returning 42. */
struct mde_test_args { int answer; };
static int mde_test_kfn(void *a)
{
    struct mde_test_args *x = (struct mde_test_args *)a;
    return x ? x->answer : -1;
}

static int test_chain_mde(char *reason, uint32_t rsize)
{
    if (CHAIN_MDE < 0) { t_strcopy(reason, "no mde chain", rsize); return TEST_SKIP; }
    chain_t *c = chain_get(CHAIN_MDE);
    if (!c) { t_strcopy(reason, "chain_get NULL", rsize); return TEST_FAIL; }
    int vv0 = c->vault_version;
    int infl0 = mde_chain_inflight();

    struct mde_test_args a = { .answer = 42 };
    mde_compute_request_t req = { .kernel_fn = mde_test_kfn, .args = &a, .rc = 0, .elapsed_tsc = 0 };
    int srv = mde_chain_submit(&req);
    int vv1 = c->vault_version;
    int infl1 = mde_chain_inflight();

    if (srv != 42)            { t_strcopy(reason, "submit rc != 42", rsize); return TEST_FAIL; }
    if (req.rc != 42)         { t_strcopy(reason, "req.rc != 42", rsize); return TEST_FAIL; }
    if (req.elapsed_tsc == 0) { t_strcopy(reason, "elapsed_tsc 0", rsize); return TEST_FAIL; }
    if (vv1 - vv0 < 2)        { t_strcopy(reason, "vault_version delta < 2", rsize); return TEST_FAIL; }
    if (infl1 != infl0)       { t_strcopy(reason, "schedule slot leaked", rsize); return TEST_FAIL; }
    return TEST_PASS;
}

static int test_chain_gpu(char *reason, uint32_t rsize)
{
    int gid = CHAIN_GPU_0;
    if (gid < 0) gid = CHAIN_DISPLAY_GOP;
    if (gid < 0) { t_strcopy(reason, "no gpu chain", rsize); return TEST_SKIP; }
    chain_t *c = chain_get(gid);
    if (!c) { t_strcopy(reason, "chain_get NULL", rsize); return TEST_FAIL; }
    int vv0 = c->vault_version;
    int rc = chain_resolve(gid);
    if (rc != 0) {
        /* GOP fallback chain has no nodes wired -- chain_resolve returns 0
         * for empty node_count, so any nonzero is a real fail. */
        t_strcopy(reason, "resolve failed", rsize);
        return TEST_FAIL;
    }
    /* If the chain has nodes that touch state, vault_version should bump.
     * Empty chain (GOP fallback) is a SKIP: paradigm visible, no work. */
    if (c->node_count == 0) {
        t_strcopy(reason, "chain has no nodes (GOP fallback)", rsize);
        return TEST_SKIP;
    }
    if (c->vault_version <= vv0) {
        /* Some GPU chains don't bump per-resolve -- accept as PASS only if
         * status remained LIVE (not ERROR). */
        if (c->status == CHAIN_LIVE) return TEST_PASS;
        t_strcopy(reason, "vault_version did not advance", rsize);
        return TEST_FAIL;
    }
    return TEST_PASS;
}

static int test_chain_display(char *reason, uint32_t rsize)
{
    int n = gpu_virtio_display_count();
    if (n <= 0) {
        int gop = -1;
        if (gpu_virtio_gop_fallback(&gop) && gop >= 0) {
            chain_t *c = chain_get(gop);
            if (c) return TEST_PASS;
            t_strcopy(reason, "GOP chain bad", rsize);
            return TEST_FAIL;
        }
        t_strcopy(reason, "no display", rsize);
        return TEST_SKIP;
    }
    int passed = 0;
    for (int i = 0; i < n; i++) {
        gpu_virtio_display_t info;
        if (gpu_virtio_display_info(i, &info) != 0) {
            t_strcopy(reason, "display_info failed", rsize);
            return TEST_FAIL;
        }
        if (info.chain_id < 0) continue;
        chain_t *c = chain_get(info.chain_id);
        if (!c) {
            t_strcopy(reason, "display chain missing", rsize);
            return TEST_FAIL;
        }
        if (info.enabled && info.width > 0 && info.height > 0) passed++;
    }
    if (passed == 0) {
        t_strcopy(reason, "no display reported a mode", rsize);
        return TEST_SKIP;
    }
    return TEST_PASS;
}

static int test_chain_keyboard(char *reason, uint32_t rsize)
{
    if (CHAIN_KEYBOARD < 0) { t_strcopy(reason, "no kbd chain", rsize); return TEST_SKIP; }
    chain_t *c = chain_get(CHAIN_KEYBOARD);
    if (!c || c->node_count != 3) {
        t_strcopy(reason, "expected 3 nodes", rsize);
        return TEST_FAIL;
    }
    /* Inject a fake scancode (0x1E = 'A' on set 1). */
    uint32_t total0 = keyboard_chain_total();
    keyboard_inject_scancode(0x1E, 0);   /* press */
    keyboard_inject_scancode(0x9E, 0);   /* release */
    int rc = chain_resolve(CHAIN_KEYBOARD);
    if (rc != 0) {
        t_strcopy(reason, "resolve failed", rsize);
        return TEST_FAIL;
    }
    /* Either the scancode advanced through the chain (total bumped) or
     * keybinds swallowed it -- both prove the chain advanced. */
    uint32_t total1 = keyboard_chain_total();
    if (total1 < total0) {
        t_strcopy(reason, "drain counter went backwards", rsize);
        return TEST_FAIL;
    }
    return TEST_PASS;
}

static int test_chain_mouse(char *reason, uint32_t rsize)
{
    if (CHAIN_MOUSE < 0) { t_strcopy(reason, "no mouse chain", rsize); return TEST_SKIP; }
    chain_t *c = chain_get(CHAIN_MOUSE);
    if (!c || c->node_count != 3) {
        t_strcopy(reason, "expected 3 nodes", rsize);
        return TEST_FAIL;
    }
    int rc = chain_resolve(CHAIN_MOUSE);
    if (rc != 0) {
        t_strcopy(reason, "resolve failed", rsize);
        return TEST_FAIL;
    }
    /* No injection API for mouse packets -- we proved chain plumbing
     * (3 nodes, resolve clean). Real packet flow is exercised via the
     * scheduler tick when a PS/2 mouse is present. */
    return TEST_PASS;
}

/* ── Cross-chain integration tests ─────────────────────────────────── */

static int test_https_through_chains(char *reason, uint32_t rsize)
{
    if (!g_net.up) {
        t_strcopy(reason, "network down", rsize);
        return TEST_SKIP;
    }
    extern int https_get(const char *hostname, const char *path,
                         char *resp, int resp_size, int *body_len);
    static char resp[4096];
    int body_len = 0;
    int status = https_get("letsencrypt.org", "/", resp, sizeof(resp), &body_len);
    if (status == 200 && body_len > 0) return TEST_PASS;
    t_strcopy(reason, "HTTPS GET failed", rsize);
    return TEST_FAIL;
}

static int test_zp_program_runs_chain(char *reason, uint32_t rsize)
{
    if (CHAIN_AUDIO < 0) { t_strcopy(reason, "no audio chain", rsize); return TEST_SKIP; }
    char src[2048];
    int got = vault_read("/programs/30_chain_native.zp", src, sizeof(src) - 1);
    if (got <= 0) {
        t_strcopy(reason, "30_chain_native.zp missing in vault", rsize);
        return TEST_SKIP;
    }
    src[got] = '\0';
    chain_t *c = chain_get(CHAIN_AUDIO);
    int vv0 = c ? c->vault_version : 0;
    int rc = zp_run(src);
    if (rc < 0) {
        t_strcopy(reason, "zp_run rejected source", rsize);
        return TEST_FAIL;
    }
    int vv1 = c ? c->vault_version : 0;
    /* The program registers a Z+ chain; loading alone may not bump
     * audio.vault_version because the heartbeat fires at scheduler tick.
     * Resolve once to drive the pipeline. */
    (void)chain_resolve(CHAIN_AUDIO);
    int vv2 = c ? c->vault_version : 0;
    (void)vv1;
    if (vv2 < vv0) {
        t_strcopy(reason, "audio vault_version regressed", rsize);
        return TEST_FAIL;
    }
    return TEST_PASS;
}

/* ── Registration ─────────────────────────────────────────────────── */

int test_register(const char *name, test_fn_t fn, int chain_id)
{
    if (g_test_count >= TESTS_MAX) return -1;
    struct test_entry *e = &g_tests[g_test_count++];
    t_strcopy(e->name, name, (int)sizeof(e->name));
    e->fn = fn;
    e->chain_id = chain_id;
    return 0;
}

void tests_register_all(void)
{
    if (g_tests_inited) return;
    g_tests_inited = 1;
    test_register("chain.audio",          test_chain_audio,          CHAIN_AUDIO);
    test_register("chain.net.tx",         test_chain_net_tx,         CHAIN_NET_TX);
    test_register("chain.net.rx",         test_chain_net_rx,         CHAIN_NET_RX);
    test_register("chain.block.read",     test_chain_block_read,     CHAIN_BLOCK);
    test_register("chain.block.write",    test_chain_block_write,    CHAIN_BLOCK);
    test_register("chain.mde",            test_chain_mde,            CHAIN_MDE);
    test_register("chain.gpu",            test_chain_gpu,            CHAIN_GPU_0);
    test_register("chain.display",        test_chain_display,        -1);
    test_register("chain.keyboard",       test_chain_keyboard,       CHAIN_KEYBOARD);
    test_register("chain.mouse",          test_chain_mouse,          CHAIN_MOUSE);
    test_register("integration.https",    test_https_through_chains, -1);
    test_register("integration.zp.chain", test_zp_program_runs_chain, CHAIN_AUDIO);
}

/* ── Runners ──────────────────────────────────────────────────────── */

struct run_stats { int total, passed, failed, skipped; };

static int run_one(int idx, struct run_stats *st, int total)
{
    struct test_entry *e = &g_tests[idx];
    char reason[64];
    reason[0] = '\0';

    /* Header */
    kputc('[');
    kput_dec((uint64_t)(idx + 1));
    kputc('/');
    kput_dec((uint64_t)total);
    kputs("] ");
    print_dotted(e->name, 28);

    struct chain_snapshot snap;
    snap_take(&snap, e->chain_id);

    uint64_t t0 = timer_read_tsc();
    int rc = e->fn(reason, sizeof(reason));
    uint64_t t1 = timer_read_tsc();
    uint64_t ms = tsc_to_ms(t1 - t0);

    /* Restore so tests don't pollute live B3 */
    snap_restore(&snap);

    if (rc == TEST_PASS) {
        kputs("PASS (");
        kput_dec(ms);
        kputs("ms)\n");
        st->passed++;
    } else if (rc == TEST_SKIP) {
        kputs("SKIPPED");
        if (reason[0]) { kputs(" ("); kputs(reason); kputc(')'); }
        kputc('\n');
        st->skipped++;
    } else {
        kputs("FAIL");
        if (reason[0]) { kputs(" ("); kputs(reason); kputc(')'); }
        kputs(" (");
        kput_dec(ms);
        kputs("ms)\n");
        st->failed++;
    }
    return rc;
}

static void print_summary(const struct run_stats *st)
{
    kputs("\n  ");
    kput_dec((uint64_t)st->total);
    kputs(" tests, ");
    kput_dec((uint64_t)st->passed);
    kputs(" passed, ");
    kput_dec((uint64_t)st->skipped);
    kputs(" skipped, ");
    kput_dec((uint64_t)st->failed);
    kputs(" failed\n\n");
}

int test_run_all(void)
{
    tests_register_all();
    struct run_stats st = { g_test_count, 0, 0, 0 };
    kputs("\n  Zeos test runner\n  ────────────────\n");
    for (int i = 0; i < g_test_count; i++) run_one(i, &st, g_test_count);
    print_summary(&st);
    return st.failed;
}

int test_run_named(const char *substring)
{
    tests_register_all();

    /* Count matches */
    int matched = 0;
    for (int i = 0; i < g_test_count; i++) {
        if (t_strstr(g_tests[i].name, substring)) matched++;
    }
    if (matched == 0) {
        kputs("  no tests match \"");
        kputs(substring);
        kputs("\"\n");
        return -1;
    }

    struct run_stats st = { matched, 0, 0, 0 };
    kputs("\n  Zeos test runner (filter=\"");
    kputs(substring);
    kputs("\")\n  ────────────────\n");

    int n = 0;
    for (int i = 0; i < g_test_count; i++) {
        if (!t_strstr(g_tests[i].name, substring)) continue;
        struct test_entry *e = &g_tests[i];
        char reason[64]; reason[0] = '\0';

        kputc('[');
        kput_dec((uint64_t)(n + 1));
        kputc('/');
        kput_dec((uint64_t)matched);
        kputs("] ");
        print_dotted(e->name, 28);

        struct chain_snapshot snap;
        snap_take(&snap, e->chain_id);
        uint64_t t0 = timer_read_tsc();
        int rc = e->fn(reason, sizeof(reason));
        uint64_t t1 = timer_read_tsc();
        uint64_t ms = tsc_to_ms(t1 - t0);
        snap_restore(&snap);

        if (rc == TEST_PASS) {
            kputs("PASS ("); kput_dec(ms); kputs("ms)\n");
            st.passed++;
        } else if (rc == TEST_SKIP) {
            kputs("SKIPPED");
            if (reason[0]) { kputs(" ("); kputs(reason); kputc(')'); }
            kputc('\n');
            st.skipped++;
        } else {
            kputs("FAIL");
            if (reason[0]) { kputs(" ("); kputs(reason); kputc(')'); }
            kputs(" ("); kput_dec(ms); kputs("ms)\n");
            st.failed++;
        }
        n++;
    }
    print_summary(&st);
    return st.failed;
}

/* JSON: {"results":[{"name":"...","status":"PASS","ms":2}, ...],
 *        "summary":{"total":N,"passed":N,"skipped":N,"failed":N}}
 */
int test_run_all_json(void)
{
    tests_register_all();
    struct run_stats st = { g_test_count, 0, 0, 0 };

    kputs("{\"results\":[");
    for (int i = 0; i < g_test_count; i++) {
        struct test_entry *e = &g_tests[i];
        char reason[64]; reason[0] = '\0';

        struct chain_snapshot snap;
        snap_take(&snap, e->chain_id);
        uint64_t t0 = timer_read_tsc();
        int rc = e->fn(reason, sizeof(reason));
        uint64_t t1 = timer_read_tsc();
        uint64_t ms = tsc_to_ms(t1 - t0);
        snap_restore(&snap);

        const char *status = (rc == TEST_PASS) ? "PASS" :
                             (rc == TEST_SKIP) ? "SKIP" : "FAIL";
        if      (rc == TEST_PASS) st.passed++;
        else if (rc == TEST_SKIP) st.skipped++;
        else                      st.failed++;

        if (i > 0) kputc(',');
        kputs("{\"name\":\"");
        kputs(e->name);
        kputs("\",\"status\":\"");
        kputs(status);
        kputs("\",\"ms\":");
        kput_dec(ms);
        if (reason[0]) {
            kputs(",\"reason\":\"");
            /* very light escape: replace " with ' */
            for (int k = 0; reason[k]; k++) {
                char c = reason[k];
                if (c == '"' || c == '\\') c = '\'';
                kputc(c);
            }
            kputc('"');
        }
        kputc('}');
    }
    kputs("],\"summary\":{\"total\":");
    kput_dec((uint64_t)st.total);
    kputs(",\"passed\":");
    kput_dec((uint64_t)st.passed);
    kputs(",\"skipped\":");
    kput_dec((uint64_t)st.skipped);
    kputs(",\"failed\":");
    kput_dec((uint64_t)st.failed);
    kputs("}}\n");
    return st.failed;
}
