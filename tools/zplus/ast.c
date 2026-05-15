/* zplus — AST utilities */
#include "ast.h"
#include <stdio.h>
#include <string.h>

const char *ast_kind_name(ast_kind_t k) {
    switch (k) {
    case AST_PROGRAM:      return "PROGRAM";
    case AST_IMPORT:       return "IMPORT";
    case AST_STRUCT:       return "STRUCT";
    case AST_FIELD:        return "FIELD";
    case AST_CHAIN:        return "CHAIN";
    case AST_CHAIN_ITEM:   return "CHAIN_ITEM";
    case AST_SIGNAL_DECL:  return "SIGNAL_DECL";
    case AST_PIPELINE:     return "PIPELINE";
    case AST_FORK:         return "FORK";
    case AST_MERGE:        return "MERGE";
    case AST_FLOW_EDGE:    return "FLOW_EDGE";
    case AST_TAP_EDGE:     return "TAP_EDGE";
    case AST_SEVER_EDGE:   return "SEVER_EDGE";
    case AST_EXCHANGE_EDGE:return "EXCHANGE_EDGE";
    case AST_CALL:         return "CALL";
    case AST_FIELD_ACCESS: return "FIELD_ACCESS";
    case AST_STRUCT_INIT:  return "STRUCT_INIT";
    case AST_IDENT:        return "IDENT";
    case AST_DOTTED:       return "DOTTED";
    case AST_INT_LIT:      return "INT_LIT";
    case AST_FLOAT_LIT:    return "FLOAT_LIT";
    case AST_STR_LIT:      return "STR_LIT";
    case AST_DURATION_LIT: return "DURATION";
    case AST_MULTIPLIER:   return "MULTIPLIER";
    case AST_GATE:         return "GATE";
    case AST_KNEE:         return "KNEE";
    case AST_DELTA:        return "DELTA";
    case AST_EMIT:         return "EMIT";
    case AST_ON_SILENCE:   return "ON_SILENCE";
    case AST_ON_BLOCK:     return "ON_BLOCK";
    case AST_SUSTAINED:    return "SUSTAINED";
    case AST_WITHIN:       return "WITHIN";
    case AST_DEBOUNCE:     return "DEBOUNCE";
    case AST_THROTTLE:     return "THROTTLE";
    case AST_DECAY:        return "DECAY";
    case AST_TICK:         return "TICK";
    case AST_RATE:         return "RATE";
    case AST_DELTA_EXPR:   return "DELTA_EXPR";
    case AST_TYPEREF:      return "TYPEREF";
    case AST_BINARY:       return "BINARY";
    case AST_UNARY:        return "UNARY";
    case AST_ARG:          return "ARG";
    case AST_LIST:         return "LIST";
    default:               return "?";
    }
}

static void indent(int n) { for (int i = 0; i < n*2; i++) putchar(' '); }

void ast_dump(ast_node_t *n, int depth) {
    if (!n) return;
    indent(depth);
    printf("%s", ast_kind_name(n->kind));

    switch (n->kind) {
    case AST_IDENT:       printf(" \"%s\"", n->u.ident.name ? n->u.ident.name : ""); break;
    case AST_DOTTED: {
        printf(" \"");
        for (int i = 0; i < n->u.dotted.nparts; i++) {
            if (i) putchar('.');
            printf("%s", n->u.dotted.parts[i]);
        }
        printf("\""); break;
    }
    case AST_INT_LIT:     printf(" %ld", n->u.ival.ival); break;
    case AST_FLOAT_LIT:   printf(" %g",  n->u.fval.fval); break;
    case AST_STR_LIT:     printf(" \"%s\"", n->u.sval.s ? n->u.sval.s : ""); break;
    case AST_DURATION_LIT:printf(" %ldms", n->u.ival.ival); break;
    case AST_MULTIPLIER:  printf(" %ldx", n->u.ival.ival); break;
    case AST_IMPORT:      printf(" %s as %s",
                                  n->u.import.module ? n->u.import.module : "?",
                                  n->u.import.alias  ? n->u.import.alias  : "?"); break;
    case AST_STRUCT:      printf(" %s (%d fields)", n->u.strct.name, n->u.strct.nfields); break;
    case AST_FIELD:       printf(" %s", n->u.field.name ? n->u.field.name : "?"); break;
    case AST_CHAIN:       printf(" %s (%d items)", n->u.chain.name, n->u.chain.nitems); break;
    case AST_CHAIN_ITEM:  printf(" %s:", n->u.chain_item.label ? n->u.chain_item.label : "?"); break;
    case AST_SIGNAL_DECL: printf(" %s", n->u.sig_decl.name ? n->u.sig_decl.name : "?"); break;
    case AST_PIPELINE:    printf(" (%d stages)", n->u.pipeline.count); break;
    case AST_FORK:        printf(" (%d branches)", n->u.fork.count); break;
    case AST_TYPEREF:     printf(" %s", n->u.typeref.name ? n->u.typeref.name : "?"); break;
    case AST_BINARY: {
        const char *ops[] = {"add","sub","mul","div","==","!=","<",">","<=",">=","~","&&","||"};
        printf(" %s", (n->u.binary.op < 13) ? ops[n->u.binary.op] : "?"); break;
    }
    case AST_ARG: if (n->u.arg.label) printf(" %s:", n->u.arg.label); break;
    case AST_STRUCT_INIT: printf(" %s{}", n->u.sinit.type_name ? n->u.sinit.type_name : ""); break;
    default: break;
    }
    printf("  @%d:%d\n", n->line, n->col);

    /* recurse into children */
    switch (n->kind) {
    case AST_PROGRAM:
    case AST_LIST: {
        ast_node_t *c = n->u.list.items;
        while (c) { ast_dump(c, depth+1); c = c->next; }
        break;
    }
    case AST_STRUCT: {
        ast_node_t *f = n->u.strct.fields;
        while (f) { ast_dump(f, depth+1); f = f->next; }
        break;
    }
    case AST_FIELD:
        if (n->u.field.type) ast_dump(n->u.field.type, depth+1);
        break;
    case AST_CHAIN: {
        ast_node_t *c = n->u.chain.items;
        while (c) { ast_dump(c, depth+1); c = c->next; }
        break;
    }
    case AST_CHAIN_ITEM:
        ast_dump(n->u.chain_item.pipeline, depth+1);
        break;
    case AST_SIGNAL_DECL:
        ast_dump(n->u.sig_decl.rhs, depth+1);
        break;
    case AST_PIPELINE:
        for (int i = 0; i < n->u.pipeline.count; i++) {
            static const char *enames[] = {"->","~>","-x>","<->"};
            if (i > 0) { indent(depth+1); printf("%s\n", enames[n->u.pipeline.edges[i-1]]); }
            ast_dump(n->u.pipeline.stages[i], depth+1);
        }
        break;
    case AST_FORK:
        for (int i = 0; i < n->u.fork.count; i++)
            ast_dump(n->u.fork.branches[i], depth+1);
        break;
    case AST_CALL:
        ast_dump(n->u.call.func, depth+1);
        { ast_node_t *a = n->u.call.args; while (a) { ast_dump(a, depth+1); a = a->next; } }
        break;
    case AST_GATE: case AST_EMIT: case AST_ON_SILENCE: case AST_DEBOUNCE:
    case AST_THROTTLE: case AST_DECAY: case AST_TICK: case AST_RATE:
    case AST_SUSTAINED: case AST_WITHIN: case AST_DELTA_EXPR:
        ast_dump(n->u.op.expr, depth+1);
        break;
    case AST_BINARY:
        ast_dump(n->u.binary.left, depth+1);
        ast_dump(n->u.binary.right, depth+1);
        break;
    case AST_UNARY:
        ast_dump(n->u.unary.operand, depth+1);
        break;
    case AST_ARG:
        ast_dump(n->u.arg.val, depth+1);
        break;
    default: break;
    }
}
