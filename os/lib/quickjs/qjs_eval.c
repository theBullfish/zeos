/*
 * qjs_eval.c — thin Zeos entry point around QuickJS.
 *
 * Owns one lazily-created JSRuntime/JSContext (full standard intrinsics:
 * Object/Array/Math/JSON/String/…), evaluates a source string, and returns the
 * result (or the exception message) as text. This is what the `js` shell
 * command and, later, the browser's <script> execution call into.
 */
#include "quickjs.h"

extern unsigned long strlen(const char *s);

static JSRuntime *g_rt;
static JSContext *g_ctx;

static void copy_out(char *out, int outlen, const char *s)
{
    if (!out || outlen <= 0) return;
    int i = 0;
    if (s) for (; s[i] && i < outlen - 1; i++) out[i] = s[i];
    out[i] = 0;
}

/* Ensure the runtime exists. Returns 0 on success. */
static int ensure_ctx(void)
{
    if (g_ctx) return 0;
    g_rt = JS_NewRuntime();
    if (!g_rt) return -1;
    g_ctx = JS_NewContext(g_rt);      /* base + standard intrinsics */
    if (!g_ctx) return -1;
    return 0;
}

/* Shared context accessor for the DOM bindings (qjs_dom.c). */
JSContext *zeos_js_context(void)
{
    if (ensure_ctx() != 0) return 0;
    return g_ctx;
}

/*
 * Evaluate `code` as global JS. On success writes the result's string form to
 * `out` and returns 0. On a JS exception writes the error text and returns -1.
 * Returns -2 if the engine couldn't be created.
 */
int zeos_js_eval(const char *code, char *out, int outlen)
{
    if (ensure_ctx() != 0) { copy_out(out, outlen, "engine init failed"); return -2; }

    JSValue val = JS_Eval(g_ctx, code, strlen(code), "<shell>", JS_EVAL_TYPE_GLOBAL);
    int rc;
    if (JS_IsException(val)) {
        JSValue exc = JS_GetException(g_ctx);
        const char *s = JS_ToCString(g_ctx, exc);
        copy_out(out, outlen, s ? s : "exception");
        if (s) JS_FreeCString(g_ctx, s);
        JS_FreeValue(g_ctx, exc);
        rc = -1;
    } else {
        const char *s = JS_ToCString(g_ctx, val);
        copy_out(out, outlen, s ? s : "undefined");
        if (s) JS_FreeCString(g_ctx, s);
        rc = 0;
    }
    JS_FreeValue(g_ctx, val);
    return rc;
}
