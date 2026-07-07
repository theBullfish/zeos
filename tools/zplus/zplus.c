/* zplus — Z+ compiler driver
 *
 * Usage:  zplus <file.zp>                 → emit <file.zp.c> + <file.zp.h>
 *         zplus --dump-ast   <file.zp>    → print AST to stdout
 *         zplus --dump-sfg   <file.zp>    → print SFG to stdout
 *         zplus --stdout     <file.zp>    → emit C to stdout (no .c file)
 *         zplus --run        <file.zp>    → compile, link with stubs, exec init()
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lexer.h"
#include "parser.h"
#include "ir.h"
#include "codegen.h"
#include "sfg_serialize.h"

/* ── read file ───────────────────────────────────────────────────── */
static char *read_file(const char *path, int *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    char *buf = (char*)malloc((size_t)(sz + 1));
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f); free(buf); return NULL;
    }
    buf[sz] = '\0'; fclose(f);
    *len_out = (int)sz;
    return buf;
}

/* ── emit output files ───────────────────────────────────────────── */
static int emit_files(sfg_program_t *g, const char *zp_path, int to_stdout) {
    if (to_stdout) {
        cg_result_t cres; memset(&cres, 0, sizeof(cres));
        codegen_emit(g, stdout, &cres);
        for (int i = 0; i < cres.nerrors; i++)
            fprintf(stderr, "codegen: %s\n", cres.errors[i].msg);
        return cres.nerrors ? 1 : 0;
    }

    /* build output paths */
    char c_path[512], h_path[512];
    snprintf(c_path, sizeof(c_path), "%s.c", zp_path);
    snprintf(h_path, sizeof(h_path), "%s.h", zp_path);

    FILE *fc = fopen(c_path, "w");
    if (!fc) { perror(c_path); return 1; }
    FILE *fh = fopen(h_path, "w");
    if (!fh) { fclose(fc); perror(h_path); return 1; }

    cg_result_t cres; memset(&cres, 0, sizeof(cres));
    codegen_emit(g, fc, &cres);
    codegen_emit_header(g, fh);

    fclose(fc); fclose(fh);

    for (int i = 0; i < cres.nerrors; i++)
        fprintf(stderr, "codegen: %s\n", cres.errors[i].msg);

    if (!cres.nerrors)
        fprintf(stderr, "zplus: wrote %s  %s\n", c_path, h_path);

    return cres.nerrors ? 1 : 0;
}

/* ── main ────────────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "zplus — Z+ compiler\n"
            "usage: zplus [--dump-ast] [--dump-sfg] [--stdout] <file.zp>\n");
        return 1;
    }

    int dump_ast = 0, dump_sfg = 0, to_stdout = 0, emit_zir = 0;
    const char *input_path = NULL;

    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "--dump-ast") == 0) dump_ast   = 1;
        else if (strcmp(argv[i], "--dump-sfg") == 0) dump_sfg   = 1;
        else if (strcmp(argv[i], "--emit-zir") == 0) emit_zir   = 1;
        else if (strcmp(argv[i], "--stdout")   == 0) to_stdout  = 1;
        else if (argv[i][0] != '-')                   input_path = argv[i];
    }

    if (!input_path) {
        fprintf(stderr, "zplus: no input file\n"); return 1;
    }

    /* ── read ──────────────────────────────────────────────────── */
    int src_len = 0;
    char *src = read_file(input_path, &src_len);
    if (!src) return 1;

    /* ── parse ─────────────────────────────────────────────────── */
    parser_t parser; parser_init(&parser);
    ast_node_t *ast = parse(&parser, src, src_len);

    if (parser.nerrors) {
        for (int i = 0; i < parser.nerrors; i++)
            fprintf(stderr, "parse: %s\n", parser.errors[i]);
    }

    if (!ast) {
        fprintf(stderr, "zplus: parse failed\n");
        parser_free(&parser); free(src); return 1;
    }

    if (dump_ast) {
        ast_dump(ast, 0);
        parser_free(&parser); free(src); return 0;
    }

    /* ── lower to SFG ──────────────────────────────────────────── */
    ir_result_t ires; memset(&ires, 0, sizeof(ires));
    sfg_program_t *g = ir_lower(ast, &parser.arena, input_path, &ires);

    if (ires.nerrors) {
        for (int i = 0; i < ires.nerrors; i++)
            fprintf(stderr, "ir: %d:%d: %s\n",
                    ires.errors[i].line, ires.errors[i].col, ires.errors[i].msg);
    }

    if (!g) {
        fprintf(stderr, "zplus: IR lowering failed\n");
        parser_free(&parser); free(src); return 1;
    }

    if (dump_sfg) {
        sfg_dump(g);
        parser_free(&parser); free(src); return 0;
    }

    if (emit_zir) {
        sfg_to_zir(g, stdout);
        parser_free(&parser); free(src); return 0;
    }

    /* ── codegen ───────────────────────────────────────────────── */
    int rc = emit_files(g, input_path, to_stdout);

    parser_free(&parser); free(src);
    return parser.nerrors ? 1 : rc;
}
