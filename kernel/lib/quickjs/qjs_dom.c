/*
 * qjs_dom.c — bind Zeos's browser DOM (browser.c) to QuickJS so <script> tags
 * can read and mutate the page. This is what makes pages *alive*.
 *
 * Exposes, on the shared JS context:
 *   console.log / console.error            -> serial
 *   document.getElementById(id)            -> Element | null
 *   Element.textContent  (get / set)       -> text, set re-renders
 *   Element.innerText    (get / set)       -> alias
 *   Element.tagName / .id                  -> string
 *   Element.getAttribute / setAttribute    -> fixed attribute slots
 *
 * DOM mutation goes through browser.c helpers (it owns the node pool) which set
 * a dirty flag; browser_navigate re-lays-out + repaints after scripts run.
 */
#include "quickjs.h"

/* dom_node_t is opaque here — browser.c owns it; we only pass pointers. */
typedef struct dom_node dom_node_t;

extern unsigned long strlen(const char *s);
extern void kputc(char c);
extern void kputs(const char *s);

/* browser.c DOM API */
extern dom_node_t  *dom_get_by_id(dom_node_t *root, const char *id);
extern const char  *dom_get_text_content(dom_node_t *el);
extern void         dom_set_text_content(dom_node_t *el, const char *s);
extern const char  *dom_get_attr(dom_node_t *el, const char *name);
extern void         dom_set_attr(dom_node_t *el, const char *name, const char *val);
extern const char  *dom_node_tag(dom_node_t *el);
extern const char  *dom_node_id(dom_node_t *el);
/* child walk for script execution */
extern dom_node_t  *dom_first_child(dom_node_t *el);
extern dom_node_t  *dom_next_sibling(dom_node_t *el);
extern const char  *dom_script_src(dom_node_t *el);

extern JSContext *zeos_js_context(void);

#define countof(x) (sizeof(x) / sizeof((x)[0]))

static JSClassID  js_element_class_id;
static int        g_dom_setup;
static dom_node_t *g_dom_root;

/* ── Element property/method implementations ── */
static JSValue el_get_textContent(JSContext *ctx, JSValueConst this_val)
{
    dom_node_t *el = JS_GetOpaque(this_val, js_element_class_id);
    return JS_NewString(ctx, dom_get_text_content(el));
}
static JSValue el_set_textContent(JSContext *ctx, JSValueConst this_val, JSValueConst val)
{
    dom_node_t *el = JS_GetOpaque(this_val, js_element_class_id);
    const char *s = JS_ToCString(ctx, val);
    dom_set_text_content(el, s ? s : "");
    if (s) JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}
static JSValue el_get_tagName(JSContext *ctx, JSValueConst this_val)
{
    dom_node_t *el = JS_GetOpaque(this_val, js_element_class_id);
    return JS_NewString(ctx, dom_node_tag(el));
}
static JSValue el_get_id(JSContext *ctx, JSValueConst this_val)
{
    dom_node_t *el = JS_GetOpaque(this_val, js_element_class_id);
    return JS_NewString(ctx, dom_node_id(el));
}
static JSValue el_getAttribute(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc;
    dom_node_t *el = JS_GetOpaque(this_val, js_element_class_id);
    const char *name = JS_ToCString(ctx, argv[0]);
    const char *v = dom_get_attr(el, name ? name : "");
    JSValue r = v ? JS_NewString(ctx, v) : JS_NULL;
    if (name) JS_FreeCString(ctx, name);
    return r;
}
static JSValue el_setAttribute(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc;
    dom_node_t *el = JS_GetOpaque(this_val, js_element_class_id);
    const char *name = JS_ToCString(ctx, argv[0]);
    const char *val  = JS_ToCString(ctx, argv[1]);
    dom_set_attr(el, name ? name : "", val ? val : "");
    if (name) JS_FreeCString(ctx, name);
    if (val)  JS_FreeCString(ctx, val);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry element_proto_funcs[] = {
    JS_CGETSET_DEF("textContent", el_get_textContent, el_set_textContent),
    JS_CGETSET_DEF("innerText",   el_get_textContent, el_set_textContent),
    JS_CGETSET_DEF("tagName",     el_get_tagName, NULL),
    JS_CGETSET_DEF("id",          el_get_id, NULL),
    JS_CFUNC_DEF("getAttribute", 1, el_getAttribute),
    JS_CFUNC_DEF("setAttribute", 2, el_setAttribute),
};

static JSValue wrap_element(JSContext *ctx, dom_node_t *el)
{
    if (!el) return JS_NULL;
    JSValue obj = JS_NewObjectClass(ctx, js_element_class_id);
    if (JS_IsException(obj)) return obj;
    JS_SetOpaque(obj, el);
    return obj;
}

/* ── document + console ── */
static JSValue doc_getElementById(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc;
    const char *id = JS_ToCString(ctx, argv[0]);
    dom_node_t *el = dom_get_by_id(g_dom_root, id ? id : "");
    if (id) JS_FreeCString(ctx, id);
    return wrap_element(ctx, el);
}
static JSValue console_log(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    kputs("  [js] ");
    for (int i = 0; i < argc; i++) {
        if (i) kputc(' ');
        const char *s = JS_ToCString(ctx, argv[i]);
        if (s) { kputs(s); JS_FreeCString(ctx, s); }
    }
    kputc('\n');
    return JS_UNDEFINED;
}

/* Lazily install the Element class + document/console globals. */
static void dom_setup(JSContext *ctx)
{
    if (g_dom_setup) return;
    g_dom_setup = 1;

    JSRuntime *rt = JS_GetRuntime(ctx);
    JS_NewClassID(&js_element_class_id);
    JSClassDef def; def.class_name = "Element"; def.finalizer = 0;
    def.gc_mark = 0; def.call = 0; def.exotic = 0;
    JS_NewClass(rt, js_element_class_id, &def);
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, element_proto_funcs, countof(element_proto_funcs));
    JS_SetClassProto(ctx, js_element_class_id, proto);

    JSValue global = JS_GetGlobalObject(ctx);

    JSValue console = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, console, "log",   JS_NewCFunction(ctx, console_log, "log", 1));
    JS_SetPropertyStr(ctx, console, "error", JS_NewCFunction(ctx, console_log, "error", 1));
    JS_SetPropertyStr(ctx, console, "warn",  JS_NewCFunction(ctx, console_log, "warn", 1));
    JS_SetPropertyStr(ctx, global, "console", console);

    JSValue document = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, document, "getElementById",
                      JS_NewCFunction(ctx, doc_getElementById, "getElementById", 1));
    JS_SetPropertyStr(ctx, global, "document", document);

    JS_FreeValue(ctx, global);
}

/* Recursively evaluate every <script> in the tree. */
static void run_scripts(JSContext *ctx, dom_node_t *node)
{
    for (dom_node_t *c = dom_first_child(node); c; c = dom_next_sibling(c)) {
        const char *src = dom_script_src(c);
        if (src && src[0]) {
            JSValue v = JS_Eval(ctx, src, strlen(src), "<script>", JS_EVAL_TYPE_GLOBAL);
            if (JS_IsException(v)) {
                JSValue e = JS_GetException(ctx);
                const char *s = JS_ToCString(ctx, e);
                kputs("  JS <script> error: "); kputs(s ? s : "?"); kputc('\n');
                if (s) JS_FreeCString(ctx, s);
                JS_FreeValue(ctx, e);
            }
            JS_FreeValue(ctx, v);
        }
        run_scripts(ctx, c);
    }
}

/* Entry point called by browser.c after a page's DOM is built. */
void zeos_js_run_page(dom_node_t *root)
{
    JSContext *ctx = zeos_js_context();
    if (!ctx || !root) return;
    dom_setup(ctx);
    g_dom_root = root;
    run_scripts(ctx, root);
}
