/*
 * Zeos — build-zeos rule runner. Real end-to-end execution.
 *
 * What this file does that the previous selftest path didn't:
 *   - Holds a rule table mirroring programs/build-zeos.zp.
 *   - On `build hello`, marks the rule RUNNING in build_polish, runs
 *     the command natively (echo / wc -l interpreters), writes the
 *     captured stdout to a vault output path, marks DONE, records
 *     elapsed ms.
 *   - Resolves the dependency between count_chains and hello so the
 *     dependent rule sees the upstream output.
 *   - `build clean` removes every registered rule's output from vault.
 *
 * Why "echo / wc -l interpreters" and not a real shell:
 *   Zeos doesn't ship a POSIX shell — cmd.run dispatches to
 *   shell_dispatch_external which is the in-tree command pump (it
 *   emits the request to the journal but doesn't execute external
 *   processes — there is no userspace process model). To produce the
 *   real bytes the build is supposed to materialize, we interpret the
 *   command line ourselves. This is the right tradeoff for the
 *   primitive: every rule's command is data inside a typed chain, and
 *   the runner that consumes it is part of the kernel's chain
 *   resolver. Future rules add interpreters; the rule registration
 *   API doesn't change.
 */

#include "build_runner.h"
#include "build_polish.h"
#include "vault.h"
#include "timeofday.h"
#include "kprint.h"

#define BR_MAX_RULES   8
#define BR_NAME_MAX    32
#define BR_PATH_MAX    96
#define BR_CMD_MAX    192
#define BR_OUT_MAX   1024

typedef struct {
    int  used;
    int  polish_id;
    char name[BR_NAME_MAX];
    char inputs[BR_PATH_MAX];   /* CSV of dependency rule names */
    char output[BR_PATH_MAX];   /* vault path */
    char cmd[BR_CMD_MAX];
    int  done;                  /* 1 once successfully built */
} br_rule_t;

static br_rule_t g_rules[BR_MAX_RULES];
static int  g_rule_count = 0;
static int  g_initialized = 0;
static uint32_t g_last_ms = 0;

static int  br_streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}
static int  br_strlen(const char *s) { int n=0; while (s && s[n]) n++; return n; }
static void br_strcpy(char *d, const char *s, int max) {
    int i = 0; while (s && s[i] && i + 1 < max) { d[i] = s[i]; i++; } d[i] = 0;
}

static br_rule_t *find_rule(const char *name) {
    for (int i = 0; i < BR_MAX_RULES; i++)
        if (g_rules[i].used && br_streq(g_rules[i].name, name)) return &g_rules[i];
    return 0;
}

static int register_rule(const char *name, const char *inputs,
                         const char *output, const char *cmd) {
    if (g_rule_count >= BR_MAX_RULES) return -1;
    int slot = -1;
    for (int i = 0; i < BR_MAX_RULES; i++) if (!g_rules[i].used) { slot = i; break; }
    if (slot < 0) return -1;
    g_rules[slot].used = 1;
    br_strcpy(g_rules[slot].name,   name,   BR_NAME_MAX);
    br_strcpy(g_rules[slot].inputs, inputs, BR_PATH_MAX);
    br_strcpy(g_rules[slot].output, output, BR_PATH_MAX);
    br_strcpy(g_rules[slot].cmd,    cmd,    BR_CMD_MAX);
    g_rules[slot].done = 0;
    g_rules[slot].polish_id =
        build_polish_rule_add(name, inputs, output, cmd);
    g_rule_count++;
    return slot;
}

void build_runner_init(void) {
    if (g_initialized) return;
    g_initialized = 1;
    /* Mirrors programs/build-zeos.zp seed rules. */
    register_rule("hello",
                  "",
                  "/build/hello.txt",
                  "echo hello world");
    register_rule("count_chains",
                  "hello",
                  "/build/hello.lines",
                  "wc -l /build/hello.txt");
}

/* ── Tiny "shell" interpreter ── */

static int starts_with(const char *s, const char *p) {
    while (*p) { if (*s != *p) return 0; s++; p++; }
    return 1;
}

/* Run a command; write stdout into out (NUL-terminated). Returns
 * bytes written or -1. Recognises:
 *     echo <text...>
 *     wc -l <vault-path>
 */
static int exec_cmd(const char *cmd, char *out, int out_max) {
    if (!cmd || !out || out_max < 1) return -1;
    out[0] = 0;
    /* skip leading spaces */
    while (*cmd == ' ') cmd++;
    if (starts_with(cmd, "echo ")) {
        const char *p = cmd + 5;
        int n = 0;
        while (*p && n < out_max - 2) out[n++] = *p++;
        out[n++] = '\n';
        out[n] = 0;
        return n;
    }
    if (starts_with(cmd, "wc -l ")) {
        const char *p = cmd + 6;
        char path[BR_PATH_MAX];
        int pi = 0;
        while (*p && *p != ' ' && pi < BR_PATH_MAX - 1) path[pi++] = *p++;
        path[pi] = 0;
        if (vault_exists(path) <= 0) return -1;
        char buf[BR_OUT_MAX];
        int got = vault_read(path, buf, sizeof(buf) - 1);
        if (got < 0) return -1;
        if (got > (int)sizeof(buf) - 1) got = sizeof(buf) - 1;
        buf[got] = 0;
        int lines = 0;
        for (int i = 0; i < got; i++) if (buf[i] == '\n') lines++;
        /* Format: "<count> <path>\n" */
        int n = 0;
        char num[16]; int ni = 0;
        if (lines == 0) num[ni++] = '0';
        else { int v = lines; while (v > 0 && ni < 15) { num[ni++] = '0' + (v % 10); v /= 10; } }
        while (ni > 0 && n < out_max - 2) out[n++] = num[--ni];
        if (n < out_max - 2) out[n++] = ' ';
        for (int i = 0; path[i] && n < out_max - 2; i++) out[n++] = path[i];
        if (n < out_max - 2) out[n++] = '\n';
        out[n] = 0;
        return n;
    }
    /* Unknown command — emit a clear error but don't pretend success. */
    return -1;
}

/* ── Rule execution with dep resolution ── */

static int run_rule_recursive(br_rule_t *r, int depth) {
    if (!r) return -1;
    if (depth > BR_MAX_RULES) return -2;  /* cycle guard */
    if (r->done) return 0;
    /* Run dependencies first. */
    const char *p = r->inputs;
    while (*p) {
        char dep[BR_NAME_MAX];
        int n = 0;
        while (*p && *p != ',' && n < BR_NAME_MAX - 1) dep[n++] = *p++;
        dep[n] = 0;
        if (n > 0) {
            br_rule_t *d = find_rule(dep);
            if (!d) {
                /* Dependency isn't a known rule — treat as "external"
                 * and skip; in a fuller implementation this would map
                 * to fs_event paths. */
            } else {
                int rc = run_rule_recursive(d, depth + 1);
                if (rc != 0) return rc;
            }
        }
        if (*p == ',') p++;
    }
    /* Mark RUNNING in polish. */
    if (r->polish_id >= 0) build_polish_rule_state(r->polish_id, BP_RUNNING, 0);
    uint32_t t0 = tod_now_millis();
    char captured[BR_OUT_MAX];
    int wrote = exec_cmd(r->cmd, captured, sizeof(captured));
    if (wrote < 0) {
        if (r->polish_id >= 0) build_polish_rule_state(r->polish_id, BP_FAILED,
                                                       tod_now_millis() - t0);
        if (r->polish_id >= 0)
            build_polish_rule_stderr(r->polish_id, "command failed (no interpreter)");
        return -3;
    }
    /* Materialize output. */
    if (vault_exists(r->output) <= 0) {
        if (vault_create(r->output, VAULT_TIER_REFERENCE) < 0) {
            if (r->polish_id >= 0)
                build_polish_rule_state(r->polish_id, BP_FAILED,
                                        tod_now_millis() - t0);
            return -3;
        }
    }
    if (vault_write(r->output, captured, (uint32_t)wrote) < 0) {
        if (r->polish_id >= 0)
            build_polish_rule_state(r->polish_id, BP_FAILED,
                                    tod_now_millis() - t0);
        return -3;
    }
    uint32_t elapsed = tod_now_millis() - t0;
    g_last_ms = elapsed;
    if (r->polish_id >= 0) build_polish_rule_state(r->polish_id, BP_DONE, elapsed);
    r->done = 1;
    return 0;
}

int build_runner_run(const char *name) {
    build_runner_init();
    br_rule_t *r = find_rule(name);
    if (!r) return -1;
    return run_rule_recursive(r, 0);
}

int build_runner_clean(void) {
    build_runner_init();
    int n = 0;
    for (int i = 0; i < BR_MAX_RULES; i++) {
        if (!g_rules[i].used) continue;
        if (vault_exists(g_rules[i].output) > 0) {
            if (vault_delete(g_rules[i].output) == 0) n++;
        }
        g_rules[i].done = 0;
        if (g_rules[i].polish_id >= 0)
            build_polish_rule_state(g_rules[i].polish_id, BP_PENDING, 0);
    }
    return n;
}

int      build_runner_rule_count(void) { build_runner_init(); return g_rule_count; }
uint32_t build_runner_last_ms(void)    { return g_last_ms; }
