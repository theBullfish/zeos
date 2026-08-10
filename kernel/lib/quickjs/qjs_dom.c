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
/* createElement / appendChild / body */
extern dom_node_t  *dom_create_element(const char *tag);
extern dom_node_t  *dom_create_text(const char *s);
extern void         dom_append_child(dom_node_t *parent, dom_node_t *child);
extern dom_node_t  *dom_get_body(dom_node_t *root);
extern int          zeos_http_fetch(const char *url, char *body_out, int max, int *status_out);
extern dom_node_t  *dom_query(dom_node_t *root, const char *sel);
extern void         dom_query_all(dom_node_t *node, const char *sel,
                                  void (*visit)(dom_node_t *, void *), void *ctx);
extern void         dom_set_inner_html(dom_node_t *el, const char *html);
extern void         dom_get_inner_html(dom_node_t *el, char *buf, int max);
extern int          dom_class_contains(dom_node_t *el, const char *cls);
extern void         dom_class_add(dom_node_t *el, const char *cls);
extern void         dom_class_remove(dom_node_t *el, const char *cls);
extern void         dom_class_toggle(dom_node_t *el, const char *cls);
extern void         dom_remove(dom_node_t *el);
extern dom_node_t  *dom_parent(dom_node_t *el);

extern JSContext *zeos_js_context(void);

#define countof(x) (sizeof(x) / sizeof((x)[0]))

static JSClassID  js_element_class_id;
static JSClassID  js_classlist_class_id;
static int        g_dom_setup;
static dom_node_t *g_dom_root;
static JSValue     g_document;

/* Event listeners live on the DOM node (the JS Element wrapper is recreated on
 * each getElementById), so we key the registry by dom_node_t*. */
#define MAX_LISTENERS 128
static struct { dom_node_t *node; char type[16]; JSValue fn; } g_listeners[MAX_LISTENERS];
static int g_nlisteners;

static int type_eq(const char *a, const char *b)
{
    int i = 0; for (; a[i] && b[i]; i++) if (a[i] != b[i]) return 0;
    return a[i] == b[i];
}

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

static JSValue el_addEventListener(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc;
    dom_node_t *el = JS_GetOpaque(this_val, js_element_class_id);
    const char *type = JS_ToCString(ctx, argv[0]);
    if (el && type && JS_IsFunction(ctx, argv[1]) && g_nlisteners < MAX_LISTENERS) {
        g_listeners[g_nlisteners].node = el;
        int i = 0; for (; type[i] && i < 15; i++) g_listeners[g_nlisteners].type[i] = type[i];
        g_listeners[g_nlisteners].type[i] = 0;
        g_listeners[g_nlisteners].fn = JS_DupValue(ctx, argv[1]);
        g_nlisteners++;
    }
    if (type) JS_FreeCString(ctx, type);
    return JS_UNDEFINED;
}

static JSValue wrap_element(JSContext *ctx, dom_node_t *el);

static JSValue el_appendChild(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc;
    dom_node_t *parent = JS_GetOpaque(this_val, js_element_class_id);
    dom_node_t *child  = JS_GetOpaque(argv[0], js_element_class_id);
    dom_append_child(parent, child);
    return JS_DupValue(ctx, argv[0]);   /* DOM appendChild returns the child */
}

static JSValue el_get_className(JSContext *ctx, JSValueConst this_val)
{
    dom_node_t *el = JS_GetOpaque(this_val, js_element_class_id);
    const char *v = dom_get_attr(el, "class");
    return JS_NewString(ctx, v ? v : "");
}
static JSValue el_set_className(JSContext *ctx, JSValueConst this_val, JSValueConst val)
{
    dom_node_t *el = JS_GetOpaque(this_val, js_element_class_id);
    const char *s = JS_ToCString(ctx, val);
    dom_set_attr(el, "class", s ? s : "");
    if (s) JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}
static JSValue el_get_innerHTML(JSContext *ctx, JSValueConst this_val)
{
    dom_node_t *el = JS_GetOpaque(this_val, js_element_class_id);
    static char buf[65536];
    dom_get_inner_html(el, buf, sizeof buf);
    return JS_NewString(ctx, buf);
}
static JSValue el_set_innerHTML(JSContext *ctx, JSValueConst this_val, JSValueConst val)
{
    dom_node_t *el = JS_GetOpaque(this_val, js_element_class_id);
    const char *s = JS_ToCString(ctx, val);
    dom_set_inner_html(el, s ? s : "");
    if (s) JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

/* querySelector on an element subtree (or document — see doc_querySelector). */
static JSValue el_querySelector(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc;
    dom_node_t *el = JS_GetOpaque(this_val, js_element_class_id);
    const char *sel = JS_ToCString(ctx, argv[0]);
    dom_node_t *found = dom_query(el, sel ? sel : "");
    if (sel) JS_FreeCString(ctx, sel);
    return wrap_element(ctx, found);
}

struct qsa_ctx { JSContext *ctx; JSValue arr; uint32_t n; };
static void qsa_visit(dom_node_t *node, void *vctx)
{
    struct qsa_ctx *q = (struct qsa_ctx *)vctx;
    JS_SetPropertyUint32(q->ctx, q->arr, q->n++, wrap_element(q->ctx, node));
}
static JSValue el_querySelectorAll(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc;
    dom_node_t *el = JS_GetOpaque(this_val, js_element_class_id);
    const char *sel = JS_ToCString(ctx, argv[0]);
    struct qsa_ctx q = { ctx, JS_NewArray(ctx), 0 };
    dom_query_all(el, sel ? sel : "", qsa_visit, &q);
    if (sel) JS_FreeCString(ctx, sel);
    return q.arr;
}

/* ── classList (opaque = the owning dom_node_t*) ── */
static JSValue cl_add(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)argc; dom_node_t *el = JS_GetOpaque(this_val, js_classlist_class_id);
    const char *s = JS_ToCString(ctx, argv[0]); dom_class_add(el, s ? s : "");
    if (s) JS_FreeCString(ctx, s); return JS_UNDEFINED;
}
static JSValue cl_remove(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)argc; dom_node_t *el = JS_GetOpaque(this_val, js_classlist_class_id);
    const char *s = JS_ToCString(ctx, argv[0]); dom_class_remove(el, s ? s : "");
    if (s) JS_FreeCString(ctx, s); return JS_UNDEFINED;
}
static JSValue cl_toggle(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)argc; dom_node_t *el = JS_GetOpaque(this_val, js_classlist_class_id);
    const char *s = JS_ToCString(ctx, argv[0]); dom_class_toggle(el, s ? s : "");
    if (s) JS_FreeCString(ctx, s); return JS_UNDEFINED;
}
static JSValue cl_contains(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)argc; dom_node_t *el = JS_GetOpaque(this_val, js_classlist_class_id);
    const char *s = JS_ToCString(ctx, argv[0]); int r = dom_class_contains(el, s ? s : "");
    if (s) JS_FreeCString(ctx, s); return JS_NewBool(ctx, r);
}
static const JSCFunctionListEntry classlist_proto_funcs[] = {
    JS_CFUNC_DEF("add", 1, cl_add),
    JS_CFUNC_DEF("remove", 1, cl_remove),
    JS_CFUNC_DEF("toggle", 1, cl_toggle),
    JS_CFUNC_DEF("contains", 1, cl_contains),
};

static JSValue el_get_classList(JSContext *ctx, JSValueConst this_val) {
    dom_node_t *el = JS_GetOpaque(this_val, js_element_class_id);
    JSValue obj = JS_NewObjectClass(ctx, js_classlist_class_id);
    JS_SetOpaque(obj, el);
    return obj;
}
static JSValue el_remove(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)argc; (void)argv; dom_remove(JS_GetOpaque(this_val, js_element_class_id));
    return JS_UNDEFINED;
}
static JSValue el_get_value(JSContext *ctx, JSValueConst this_val) {
    dom_node_t *el = JS_GetOpaque(this_val, js_element_class_id);
    const char *v = dom_get_attr(el, "value");
    return JS_NewString(ctx, v ? v : "");
}
static JSValue el_set_value(JSContext *ctx, JSValueConst this_val, JSValueConst val) {
    dom_node_t *el = JS_GetOpaque(this_val, js_element_class_id);
    const char *s = JS_ToCString(ctx, val); dom_set_attr(el, "value", s ? s : "");
    if (s) JS_FreeCString(ctx, s); return JS_UNDEFINED;
}
static JSValue el_get_parentNode(JSContext *ctx, JSValueConst this_val) {
    return wrap_element(ctx, dom_parent(JS_GetOpaque(this_val, js_element_class_id)));
}
static JSValue el_get_children(JSContext *ctx, JSValueConst this_val) {
    dom_node_t *el = JS_GetOpaque(this_val, js_element_class_id);
    JSValue arr = JS_NewArray(ctx); uint32_t n = 0;
    for (dom_node_t *c = dom_first_child(el); c; c = dom_next_sibling(c))
        if (dom_node_tag(c)[0])   /* element (text nodes have empty tag) */
            JS_SetPropertyUint32(ctx, arr, n++, wrap_element(ctx, c));
    return arr;
}

static const JSCFunctionListEntry element_proto_funcs[] = {
    JS_CGETSET_DEF("textContent", el_get_textContent, el_set_textContent),
    JS_CGETSET_DEF("innerText",   el_get_textContent, el_set_textContent),
    JS_CGETSET_DEF("innerHTML",   el_get_innerHTML, el_set_innerHTML),
    JS_CGETSET_DEF("className",   el_get_className, el_set_className),
    JS_CGETSET_DEF("tagName",     el_get_tagName, NULL),
    JS_CGETSET_DEF("id",          el_get_id, NULL),
    JS_CFUNC_DEF("getAttribute", 1, el_getAttribute),
    JS_CFUNC_DEF("setAttribute", 2, el_setAttribute),
    JS_CFUNC_DEF("addEventListener", 2, el_addEventListener),
    JS_CFUNC_DEF("appendChild", 1, el_appendChild),
    JS_CFUNC_DEF("querySelector", 1, el_querySelector),
    JS_CFUNC_DEF("querySelectorAll", 1, el_querySelectorAll),
    JS_CGETSET_DEF("classList",   el_get_classList, NULL),
    JS_CGETSET_DEF("value",       el_get_value, el_set_value),
    JS_CGETSET_DEF("parentNode",  el_get_parentNode, NULL),
    JS_CGETSET_DEF("parentElement", el_get_parentNode, NULL),
    JS_CGETSET_DEF("children",    el_get_children, NULL),
    JS_CFUNC_DEF("remove", 0, el_remove),
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
static JSValue doc_querySelector(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc;
    const char *sel = JS_ToCString(ctx, argv[0]);
    dom_node_t *found = dom_query(g_dom_root, sel ? sel : "");
    if (sel) JS_FreeCString(ctx, sel);
    return wrap_element(ctx, found);
}
static JSValue doc_querySelectorAll(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc;
    const char *sel = JS_ToCString(ctx, argv[0]);
    struct qsa_ctx q = { ctx, JS_NewArray(ctx), 0 };
    dom_query_all(g_dom_root, sel ? sel : "", qsa_visit, &q);
    if (sel) JS_FreeCString(ctx, sel);
    return q.arr;
}
static JSValue doc_createElement(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc;
    const char *tag = JS_ToCString(ctx, argv[0]);
    dom_node_t *el = dom_create_element(tag ? tag : "div");
    if (tag) JS_FreeCString(ctx, tag);
    return wrap_element(ctx, el);
}
static JSValue doc_createTextNode(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc;
    const char *s = JS_ToCString(ctx, argv[0]);
    dom_node_t *el = dom_create_text(s ? s : "");
    if (s) JS_FreeCString(ctx, s);
    return wrap_element(ctx, el);
}
/* ── fetch() ── */
static JSValue resolved_promise(JSContext *ctx, JSValue value)
{
    JSValue funcs[2];
    JSValue p = JS_NewPromiseCapability(ctx, funcs);
    JSValueConst arg = value;
    JSValue rr = JS_Call(ctx, funcs[0], JS_UNDEFINED, 1, &arg);
    JS_FreeValue(ctx, rr);
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    JS_FreeValue(ctx, value);
    return p;
}
static JSValue resp_text(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JSValue b = JS_GetPropertyStr(ctx, this_val, "_body");
    return resolved_promise(ctx, b);
}
static JSValue resp_json(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JSValue b = JS_GetPropertyStr(ctx, this_val, "_body");
    const char *s = JS_ToCString(ctx, b);
    JSValue parsed = JS_ParseJSON(ctx, s ? s : "null", s ? strlen(s) : 4, "<json>");
    if (s) JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, b);
    return resolved_promise(ctx, parsed);
}
static char g_fetch_buf[131072];   /* 128 KB response cap for JS fetch */
static JSValue js_fetch(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc;
    const char *url = JS_ToCString(ctx, argv[0]);
    int status = -1;
    int rc = url ? zeos_http_fetch(url, g_fetch_buf, sizeof g_fetch_buf, &status) : -1;
    if (url) JS_FreeCString(ctx, url);
    JSValue resp = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, resp, "status", JS_NewInt32(ctx, status));
    JS_SetPropertyStr(ctx, resp, "ok", JS_NewBool(ctx, status >= 200 && status < 300));
    JS_SetPropertyStr(ctx, resp, "_body", JS_NewString(ctx, rc == 0 ? g_fetch_buf : ""));
    JS_SetPropertyStr(ctx, resp, "text", JS_NewCFunction(ctx, resp_text, "text", 0));
    JS_SetPropertyStr(ctx, resp, "json", JS_NewCFunction(ctx, resp_json, "json", 0));
    return resolved_promise(ctx, resp);
}

/* Run the microtask queue (Promise .then callbacks). */
static void drain_jobs(JSContext *ctx)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSContext *c1;
    int guard = 0;
    while (JS_ExecutePendingJob(rt, &c1) > 0 && ++guard < 100000) { }
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

    JS_NewClassID(&js_classlist_class_id);
    JSClassDef cldef; cldef.class_name = "DOMTokenList"; cldef.finalizer = 0;
    cldef.gc_mark = 0; cldef.call = 0; cldef.exotic = 0;
    JS_NewClass(rt, js_classlist_class_id, &cldef);
    JSValue clproto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, clproto, classlist_proto_funcs, countof(classlist_proto_funcs));
    JS_SetClassProto(ctx, js_classlist_class_id, clproto);

    JSValue global = JS_GetGlobalObject(ctx);

    JSValue console = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, console, "log",   JS_NewCFunction(ctx, console_log, "log", 1));
    JS_SetPropertyStr(ctx, console, "error", JS_NewCFunction(ctx, console_log, "error", 1));
    JS_SetPropertyStr(ctx, console, "warn",  JS_NewCFunction(ctx, console_log, "warn", 1));
    JS_SetPropertyStr(ctx, global, "console", console);

    JS_SetPropertyStr(ctx, global, "fetch", JS_NewCFunction(ctx, js_fetch, "fetch", 1));

    JSValue document = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, document, "getElementById",
                      JS_NewCFunction(ctx, doc_getElementById, "getElementById", 1));
    JS_SetPropertyStr(ctx, document, "querySelector",
                      JS_NewCFunction(ctx, doc_querySelector, "querySelector", 1));
    JS_SetPropertyStr(ctx, document, "querySelectorAll",
                      JS_NewCFunction(ctx, doc_querySelectorAll, "querySelectorAll", 1));
    JS_SetPropertyStr(ctx, document, "createElement",
                      JS_NewCFunction(ctx, doc_createElement, "createElement", 1));
    JS_SetPropertyStr(ctx, document, "createTextNode",
                      JS_NewCFunction(ctx, doc_createTextNode, "createTextNode", 1));
    /* keep a reference so run_page can refresh document.body per navigation */
    g_document = JS_DupValue(ctx, document);
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
            drain_jobs(ctx);          /* run Promise .then chains (fetch, etc.) */
        }
        run_scripts(ctx, c);
    }
}

/* Drop all listeners (called on navigation — the old DOM is gone). */
void zeos_js_reset_listeners(void)
{
    JSContext *ctx = zeos_js_context();
    for (int i = 0; i < g_nlisteners; i++)
        if (ctx) JS_FreeValue(ctx, g_listeners[i].fn);
    g_nlisteners = 0;
}

/* Fire `type` listeners registered on `node`. Returns the number fired. The
 * caller (browser.c) re-lays-out if the handlers mutated the DOM. */
int zeos_js_dispatch_event(dom_node_t *node, const char *type)
{
    JSContext *ctx = zeos_js_context();
    if (!ctx || !node) return 0;
    int fired = 0;
    for (int i = 0; i < g_nlisteners; i++) {
        if (g_listeners[i].node != node || !type_eq(g_listeners[i].type, type))
            continue;
        JSValue ev = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, ev, "type", JS_NewString(ctx, type));
        JS_SetPropertyStr(ctx, ev, "target", wrap_element(ctx, node));
        JSValue r = JS_Call(ctx, g_listeners[i].fn, JS_UNDEFINED, 1, (JSValueConst *)&ev);
        if (JS_IsException(r)) {
            JSValue e = JS_GetException(ctx);
            const char *s = JS_ToCString(ctx, e);
            kputs("  JS event error: "); kputs(s ? s : "?"); kputc('\n');
            if (s) JS_FreeCString(ctx, s);
            JS_FreeValue(ctx, e);
        }
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, ev);
        fired++;
    }
    return fired;
}

/* Entry point called by browser.c after a page's DOM is built. */
void zeos_js_run_page(dom_node_t *root)
{
    JSContext *ctx = zeos_js_context();
    if (!ctx || !root) return;
    zeos_js_reset_listeners();
    dom_setup(ctx);
    g_dom_root = root;
    /* refresh document.body for this page */
    JS_SetPropertyStr(ctx, g_document, "body", wrap_element(ctx, dom_get_body(root)));
    run_scripts(ctx, root);
}
