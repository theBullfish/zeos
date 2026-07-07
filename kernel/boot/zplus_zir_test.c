/*
 * Host unit test for the ZIR loader (zplus_zir.c).
 *
 * Compiles and runs on a normal host with gcc — it exercises the SAME
 * translation unit the kernel uses, proving the kernel can ingest ZIR without
 * booting. Build:
 *     gcc -I. zplus_zir.c zplus_zir_test.c -o /tmp/zir_test
 * Run:
 *     /tmp/zir_test [real_zir_file.json]
 * Exit 0 = all assertions pass.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zplus_zir.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok   %s\n", msg); } \
    else { printf("  FAIL %s\n", msg); failures++; } \
} while (0)

static const struct zp_node_decl *find_node(const struct zp_program *p, const char *name) {
    for (int i = 0; i < p->node_count; i++)
        if (strcmp(p->nodes[i].name, name) == 0) return &p->nodes[i];
    return NULL;
}

static void test_verb_mapping(void) {
    printf("[verb -> kernel node type]\n");
    int known;
    CHECK(zir_verb_to_type("emit", NULL, &known) == ZP_EMIT && known, "emit -> ZP_EMIT");
    CHECK(zir_verb_to_type("gate", "gt", &known) == ZP_GATE_GT && known, "gate(gt) -> ZP_GATE_GT");
    CHECK(zir_verb_to_type("gate", "le", &known) == ZP_GATE_LTE && known, "gate(le) -> ZP_GATE_LTE");
    CHECK(zir_verb_to_type("str.len", NULL, &known) == ZP_STR_LEN && known, "str.len -> ZP_STR_LEN");
    CHECK(zir_verb_to_type("vault.put", NULL, &known) == ZP_VAULT_PUT && known, "vault.put -> ZP_VAULT_PUT");
    zir_verb_to_type("totally.unknown.verb", NULL, &known);
    CHECK(!known, "unknown verb marked unmapped (known==0)");
    zir_verb_to_type("gate", "match", &known);
    CHECK(!known, "gate(match) has no scalar type -> unmapped");
}

static void test_handbuilt_program(void) {
    printf("[load a hand-built ZIR program]\n");
    const char *json =
        "{\n"
        "  \"zir\": 1,\n"
        "  \"source\": \"t.zp\",\n"
        "  \"chains\": [ { \"id\": 0, \"name\": \"main\", \"masq\": \"reference\", \"parent\": -1, \"nodes\": [0,1,2] } ],\n"
        "  \"nodes\": [\n"
        "    { \"id\": 0, \"chain\": 0, \"seq\": 0, \"kind\": \"source\", \"verb\": \"emit\", \"name\": \"emit_0\", \"sig_in\": \"void\", \"sig_out\": \"signal\", \"args\": [], \"emit\": {\"int\": 42} },\n"
        "    { \"id\": 1, \"chain\": 0, \"seq\": 1, \"kind\": \"gate\", \"verb\": \"gate\", \"name\": \"gate_1\", \"sig_in\": \"signal\", \"sig_out\": \"signal\", \"args\": [], \"gate\": {\"op\": \"gt\", \"rhs\": {\"int\": 3}} },\n"
        "    { \"id\": 2, \"chain\": 0, \"seq\": 2, \"kind\": \"sink\", \"verb\": \"print\", \"name\": \"print_2\", \"sig_in\": \"signal\", \"sig_out\": \"void\", \"args\": [{\"pos\": {\"str\": \"value=\"}}] }\n"
        "  ],\n"
        "  \"edges\": [\n"
        "    { \"id\": 0, \"kind\": \"flow\", \"from\": 0, \"to\": 1, \"sig\": \"signal\" },\n"
        "    { \"id\": 1, \"kind\": \"flow\", \"from\": 1, \"to\": 2, \"sig\": \"signal\" }\n"
        "  ]\n"
        "}\n";

    struct zp_program prog;
    zir_load_result_t res;
    int rc = zir_load(json, &prog, &res);
    CHECK(rc == 0 && res.ok, "zir_load succeeds");
    CHECK(res.version == 1, "version read as 1");
    CHECK(prog.node_count == 3, "3 nodes loaded");
    CHECK(prog.edge_count == 2, "2 edges loaded");

    const struct zp_node_decl *emit = find_node(&prog, "emit_0");
    CHECK(emit && emit->type == ZP_EMIT, "emit_0 is ZP_EMIT");
    CHECK(emit && emit->int_val == 42, "emit_0 carries int_val 42");

    const struct zp_node_decl *gate = find_node(&prog, "gate_1");
    CHECK(gate && gate->type == ZP_GATE_GT, "gate_1 is ZP_GATE_GT");
    CHECK(gate && gate->int_val == 3, "gate_1 threshold is 3");

    const struct zp_node_decl *print = find_node(&prog, "print_2");
    CHECK(print && print->type == ZP_PRINT, "print_2 is ZP_PRINT");

    /* edges resolved id -> name */
    CHECK(strcmp(prog.edges[0].src, "emit_0") == 0 && strcmp(prog.edges[0].dst, "gate_1") == 0,
          "edge 0: emit_0 -> gate_1");
    CHECK(strcmp(prog.edges[1].src, "gate_1") == 0 && strcmp(prog.edges[1].dst, "print_2") == 0,
          "edge 1: gate_1 -> print_2");
}

static void test_version_guard(void) {
    printf("[reject unsupported version]\n");
    const char *json = "{ \"zir\": 99, \"nodes\": [], \"edges\": [] }";
    struct zp_program prog;
    zir_load_result_t res;
    int rc = zir_load(json, &prog, &res);
    CHECK(rc == -1 && !res.ok, "zir=99 rejected");
    CHECK(strstr(res.error, "version") != NULL, "error mentions version");
}

static void test_string_escaping(void) {
    printf("[string escapes survive the reader]\n");
    const char *json =
        "{ \"zir\": 1, \"nodes\": ["
        "{ \"verb\": \"emit\", \"name\": \"e\", \"emit\": {\"str\": \"a\\\"b\\nc\"} }"
        "], \"edges\": [] }";
    struct zp_program prog;
    zir_load_result_t res;
    int rc = zir_load(json, &prog, &res);
    CHECK(rc == 0 && prog.node_count == 1, "loads node with escaped string");
    const struct zp_node_decl *e = find_node(&prog, "e");
    CHECK(e && e->fmt[0] == 'a' && e->fmt[1] == '"' && e->fmt[2] == 'b' && e->fmt[3] == '\n',
          "escapes decoded: a\"b<newline>c");
}

/* Feed a real zplus-emit output file (optional argv[1]). */
static void test_real_file(const char *path) {
    printf("[load real zplus-emit output: %s]\n", path);
    FILE *f = fopen(path, "rb");
    if (!f) { printf("  (skip: cannot open %s)\n", path); return; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(n + 1);
    size_t got = fread(buf, 1, n, f);
    buf[got] = 0;
    fclose(f);

    struct zp_program prog;
    zir_load_result_t res;
    int rc = zir_load(buf, &prog, &res);
    printf("  loaded: rc=%d ok=%d nodes=%d edges=%d unmapped=%d truncated=%d\n",
           rc, res.ok, res.nodes_loaded, res.edges_loaded, res.unmapped, res.truncated);
    CHECK(rc == 0 && res.ok, "real ZIR file loads");
    CHECK(prog.node_count > 0, "produced at least one node");
    free(buf);
}

int main(int argc, char **argv) {
    printf("=== ZIR loader host tests ===\n");
    test_verb_mapping();
    test_handbuilt_program();
    test_version_guard();
    test_string_escaping();
    if (argc > 1) test_real_file(argv[1]);

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
