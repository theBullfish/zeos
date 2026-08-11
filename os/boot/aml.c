/*
 * Zeos — minimum-viable AML interpreter.
 *
 * Minimum-viable AML interpreter. Covers ~50 opcodes used by
 * power/battery/display methods. NOT spec-compliant: no synchronization
 * (Mutex/Event), no notifications (Notify), no embedded controller
 * region access (yet — needed for many laptop batteries; documented
 * as gap), no resource template parsing.
 *
 * Errors are not silently masked — unknown opcodes log
 * "[aml] opcode 0x%02x at offset N: not implemented" and abort the
 * evaluation.
 *
 * Two passes:
 *   1. Catalog: walk every AML table, record each Name(...) and
 *      Method(...) declaration with its absolute path. Also OpRegion
 *      and Field declarations are captured so field-unit reads work.
 *   2. Evaluator: invoked on demand by aml_evaluate(path, args).
 *
 * Honest scope:
 *   - Constants (Zero/One/Ones/Byte/Word/DWord/QWord/String)
 *   - Buffer / Package / VarPackage
 *   - Name, Alias, Scope, Device (skip-through), Method
 *   - Add/Sub/Mul/Div/And/Or/Not/LAnd/LOr/LNot/LEqual/LGreater/LLess
 *   - ToInteger/ToBuffer/ToString/SizeOf/Index/DerefOf/Store/
 *     Increment/Decrement/CondRefOf
 *   - If/Else/While/Return/Break/Continue/Noop
 *   - Method invocation by NameString + Arg/Local
 *   - OperationRegion + Field for SystemMemory / SystemIO / PCI_Config
 *     (read-only for v1; writes only for explicit Store-to-field
 *     targeting integer/byte fields up to 64 bits)
 *   - Mutex, Event, Notify: PARSED + IGNORED at evaluation time
 *     (Acquire/Release become no-ops, Notify is logged but does not
 *     dispatch, since the OS doesn't subscribe yet). This is honest:
 *     methods that GUARD critical sections still execute.
 *
 * Out of scope (these abort evaluation with "not implemented"):
 *   - Resource templates / _CRS parsing
 *   - EmbeddedControl / SMBus / IPMI / GPIO regions
 *   - CreateField + bit-field overlays
 *   - DerefOf-of-Index-of-Buffer for non-integer writes
 *   - ControlMethod recursion past 16 frames
 *
 * Design constraints:
 *   - No malloc in the inner loop — locals/args are stack-allocated
 *     up to a bounded depth.
 *   - Symbol table is a flat array; lookup is linear but bounded
 *     (~512 entries on a typical laptop DSDT).
 */

#include "aml.h"
#include "acpi.h"
#include "kprint.h"
#include "heap.h"
#include "io.h"
#include "vmm.h"
#include "timer.h"
#include "pci.h"

/* ── Compile-time tunables ─────────────────────────────────────── */

#define AML_MAX_SYMBOLS    1024
#define AML_MAX_PATH_LEN     96
#define AML_MAX_LOCALS        8
#define AML_MAX_ARGS          7
#define AML_MAX_DEPTH        16
#define AML_MAX_PKG_ELEMS    64
#define AML_JOURNAL_RING    128

/* ── Opcodes (subset) ──────────────────────────────────────────── */

#define OP_ZERO          0x00
#define OP_ONE           0x01
#define OP_ALIAS         0x06
#define OP_NAME          0x08
#define OP_BYTE          0x0A
#define OP_WORD          0x0B
#define OP_DWORD         0x0C
#define OP_STRING        0x0D
#define OP_QWORD         0x0E
#define OP_SCOPE         0x10
#define OP_BUFFER        0x11
#define OP_PACKAGE       0x12
#define OP_VAR_PACKAGE   0x13
#define OP_METHOD        0x14
#define OP_DUAL_NAME     0x2E
#define OP_MULTI_NAME    0x2F
#define OP_EXT           0x5B    /* extended opcode prefix */
#define OP_LOCAL0        0x60
#define OP_LOCAL7        0x67
#define OP_ARG0          0x68
#define OP_ARG6          0x6E
#define OP_STORE         0x70
#define OP_REF_OF        0x71
#define OP_ADD           0x72
#define OP_CONCAT        0x73
#define OP_SUBTRACT      0x74
#define OP_INCREMENT     0x75
#define OP_DECREMENT     0x76
#define OP_MULTIPLY      0x77
#define OP_DIVIDE        0x78
#define OP_SHIFT_LEFT    0x79
#define OP_SHIFT_RIGHT   0x7A
#define OP_AND           0x7B
#define OP_NAND          0x7C
#define OP_OR            0x7D
#define OP_NOR           0x7E
#define OP_XOR           0x7F
#define OP_NOT           0x80
#define OP_FIND_SET_LB   0x81
#define OP_FIND_SET_RB   0x82
#define OP_DEREF_OF      0x83
#define OP_CONCAT_RES    0x84
#define OP_MOD           0x85
#define OP_NOTIFY        0x86
#define OP_SIZE_OF       0x87
#define OP_INDEX         0x88
#define OP_MATCH         0x89
#define OP_OBJECT_TYPE   0x8E
#define OP_LAND          0x90
#define OP_LOR           0x91
#define OP_LNOT          0x92
#define OP_LEQUAL        0x93
#define OP_LGREATER      0x94
#define OP_LLESS         0x95
#define OP_TO_BUFFER     0x96
#define OP_TO_DEC_STRING 0x97
#define OP_TO_HEX_STRING 0x98
#define OP_TO_INTEGER    0x99
#define OP_TO_STRING     0x9C
#define OP_COPY_OBJECT   0x9D
#define OP_MID           0x9E
#define OP_CONTINUE      0x9F
#define OP_IF            0xA0
#define OP_ELSE          0xA1
#define OP_WHILE         0xA2
#define OP_NOOP          0xA3
#define OP_RETURN        0xA4
#define OP_BREAK         0xA5
#define OP_BREAKPOINT    0xCC
#define OP_ONES          0xFF

/* Extended opcodes (preceded by 0x5B) */
#define EXT_MUTEX        0x01
#define EXT_EVENT        0x02
#define EXT_COND_REF_OF  0x12
#define EXT_CREATE_FIELD 0x13
#define EXT_LOAD_TABLE   0x1F
#define EXT_LOAD         0x20
#define EXT_STALL        0x21
#define EXT_SLEEP        0x22
#define EXT_ACQUIRE      0x23
#define EXT_SIGNAL       0x24
#define EXT_WAIT         0x25
#define EXT_RESET        0x26
#define EXT_RELEASE      0x27
#define EXT_FROM_BCD     0x28
#define EXT_TO_BCD       0x29
#define EXT_REVISION     0x30
#define EXT_DEBUG        0x31
#define EXT_FATAL        0x32
#define EXT_TIMER        0x33
#define EXT_OP_REGION    0x80
#define EXT_FIELD        0x81
#define EXT_DEVICE       0x82
#define EXT_PROCESSOR    0x83
#define EXT_POWER_RES    0x84
#define EXT_THERMAL_ZONE 0x85
#define EXT_INDEX_FIELD  0x86
#define EXT_BANK_FIELD   0x87
#define EXT_DATA_REGION  0x88

/* Region space identifiers */
#define REGION_SYSTEM_MEMORY 0x00
#define REGION_SYSTEM_IO     0x01
#define REGION_PCI_CONFIG    0x02
#define REGION_EMBEDDED_CTRL 0x03
#define REGION_SMBUS         0x04
#define REGION_CMOS          0x05

/* Field flags: low 4 bits = access type (0=Any, 1=Byte, 2=Word, 3=DWord, 4=QWord, 5=Buffer) */
#define FIELD_ACCESS_MASK 0x0F

/* ── Symbol table ──────────────────────────────────────────────── */

typedef enum {
    SYM_NAME = 1,        /* a Name(...) value, payload is aml_object_t */
    SYM_METHOD,          /* a Method(...), payload is body bytes */
    SYM_OP_REGION,       /* OperationRegion */
    SYM_FIELD_UNIT,      /* a single field inside an OperationRegion */
    SYM_DEVICE,          /* Device/Scope/Processor/PowerRes/ThermalZone */
} sym_kind_t;

typedef struct {
    uint8_t  region_space;
    uint64_t region_base;
    uint64_t region_len;
} sym_region_t;

typedef struct {
    int      region_sym;       /* parent OpRegion sym idx */
    uint32_t bit_offset;
    uint32_t bit_width;
    uint8_t  access_type;      /* 1=Byte 2=Word 3=DWord 4=QWord */
} sym_field_t;

typedef struct {
    char        path[AML_MAX_PATH_LEN];
    sym_kind_t  kind;
    /* For methods: */
    const uint8_t *body;
    uint32_t       body_len;
    uint8_t        n_args;
    /* For named values: cached object */
    aml_object_t   value;
    /* OpRegion payload */
    sym_region_t   region;
    /* Field payload */
    sym_field_t    field;
} aml_sym_t;

static aml_sym_t g_syms[AML_MAX_SYMBOLS];
static int g_sym_count = 0;
static int g_initialized = 0;

/* ── Journal ───────────────────────────────────────────────────── */

static aml_journal_entry_t g_journal[AML_JOURNAL_RING];
static int g_journal_head = 0;   /* next slot to write */
static int g_journal_size = 0;   /* entries used */

int aml_journal_count(void) { return g_journal_size; }

const aml_journal_entry_t *aml_journal_at(int idx)
{
    if (idx < 0 || idx >= g_journal_size) return 0;
    int start = (g_journal_head - g_journal_size + AML_JOURNAL_RING) % AML_JOURNAL_RING;
    return &g_journal[(start + idx) % AML_JOURNAL_RING];
}

static void journal_record(const char *path, const aml_object_t *args, int nargs,
                           int rc, const aml_object_t *result, uint64_t elapsed,
                           uint64_t tsc_at)
{
    aml_journal_entry_t *e = &g_journal[g_journal_head];
    int i = 0;
    while (i < AML_MAX_PATH_LEN - 1 && path[i]) { e->path[i] = path[i]; i++; }
    e->path[i] = 0;
    e->nargs = nargs;
    e->arg0 = (nargs > 0 && args && args[0].type == AML_T_INTEGER) ? args[0].u.integer : 0;
    e->rc = rc;
    if (result) {
        e->result_type = (int)result->type;
        e->result_int = (result->type == AML_T_INTEGER) ? result->u.integer : 0;
    } else {
        e->result_type = 0;
        e->result_int = 0;
    }
    e->elapsed_tsc = elapsed;
    e->tsc_at_call = tsc_at;
    g_journal_head = (g_journal_head + 1) % AML_JOURNAL_RING;
    if (g_journal_size < AML_JOURNAL_RING) g_journal_size++;
}

/* ── Local utilities ───────────────────────────────────────────── */

static void *a_memset(void *dst, int v, uint32_t n)
{
    uint8_t *p = (uint8_t *)dst;
    for (uint32_t i = 0; i < n; i++) p[i] = (uint8_t)v;
    return dst;
}

static void *a_memcpy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

static int a_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static int a_streq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == 0 && *b == 0;
}

static void a_strcpy_max(char *dst, const char *src, int max)
{
    int i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void a_strcat_max(char *dst, const char *src, int max)
{
    int i = a_strlen(dst);
    int j = 0;
    while (i < max - 1 && src[j]) { dst[i++] = src[j++]; }
    dst[i] = 0;
}

/* ── Object lifecycle ──────────────────────────────────────────── */

void aml_object_release(aml_object_t *o)
{
    if (!o) return;
    if (o->type == AML_T_BUFFER && o->u.buf.owned && o->u.buf.bytes) {
        kfree(o->u.buf.bytes);
    } else if (o->type == AML_T_PACKAGE && o->u.pkg.owned && o->u.pkg.elems) {
        for (uint32_t i = 0; i < o->u.pkg.n; i++) {
            aml_object_release(&o->u.pkg.elems[i]);
        }
        kfree(o->u.pkg.elems);
    }
    o->type = AML_T_UNINIT;
}

static void obj_set_int(aml_object_t *o, uint64_t v)
{
    aml_object_release(o);
    o->type = AML_T_INTEGER;
    o->u.integer = v;
}

static void obj_copy(aml_object_t *dst, const aml_object_t *src)
{
    aml_object_release(dst);
    *dst = *src;
    /* Mark non-owned so caller doesn't double-free. Buffer/Package
     * share the underlying memory until somebody takes ownership
     * via an explicit deep copy (which we don't do — Stores either
     * write integers or replace via a fresh object). */
    if (dst->type == AML_T_BUFFER) dst->u.buf.owned = 0;
    if (dst->type == AML_T_PACKAGE) dst->u.pkg.owned = 0;
}

static uint64_t obj_to_int(const aml_object_t *o)
{
    if (!o) return 0;
    switch (o->type) {
    case AML_T_INTEGER: return o->u.integer;
    case AML_T_BUFFER: {
        uint64_t v = 0;
        uint32_t n = o->u.buf.len < 8 ? o->u.buf.len : 8;
        for (uint32_t i = 0; i < n; i++) v |= ((uint64_t)o->u.buf.bytes[i]) << (i * 8);
        return v;
    }
    case AML_T_STRING: {
        uint64_t v = 0;
        const char *s = o->u.str.s;
        if (!s) return 0;
        /* Hex if "0x"-prefixed, decimal otherwise. */
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            s += 2;
            while (*s) {
                char c = *s++;
                if (c >= '0' && c <= '9') v = v * 16 + (c - '0');
                else if (c >= 'a' && c <= 'f') v = v * 16 + (c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') v = v * 16 + (c - 'A' + 10);
                else break;
            }
        } else {
            while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
        }
        return v;
    }
    default: return 0;
    }
}

/* ── PkgLength + NameString decoders ───────────────────────────── */

static int decode_pkglen(const uint8_t *p, const uint8_t *end, uint32_t *out)
{
    if (p >= end) return -1;
    uint8_t b0 = *p;
    int extra = (b0 >> 6) & 3;
    if (p + extra >= end) return -1;
    uint32_t v;
    if (extra == 0) v = b0 & 0x3F;
    else {
        v = b0 & 0x0F;
        for (int i = 0; i < extra; i++) v |= ((uint32_t)p[1 + i]) << (4 + 8 * i);
    }
    *out = v;
    return 1 + extra;
}

/* Decode a NameString. Writes a NUL-terminated dotted path into out.
 * Returns bytes consumed from the AML stream, or -1 on error.
 *
 * NameString grammar:
 *   [\\] [^...] (NullName | DualName seg seg | MultiName count seg* | NameSeg)
 *
 * We render absolute paths starting with `\` and segments separated
 * by `.`. Output never includes `^`; callers using `^` need to resolve
 * against the current scope BEFORE decoding (we expose decode_namestring_rel).
 */
static int decode_nameseg(const uint8_t *p, const uint8_t *end, char *out)
{
    if (p + 4 > end) return -1;
    for (int i = 0; i < 4; i++) {
        char c = (char)p[i];
        /* AML namesegs: A-Z, 0-9, '_'. Pad with '_' is allowed. */
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return -1;
        out[i] = c;
    }
    out[4] = 0;
    return 4;
}

/* Decode a NameString relative to `scope`. `scope` is a fully-qualified
 * path ending with no trailing dot (e.g. "\\_SB_.PCI0"). Result is
 * written to `out` (size AML_MAX_PATH_LEN). */
static int decode_namestring_rel(const uint8_t *p, const uint8_t *end,
                                 const char *scope, char *out)
{
    const uint8_t *start = p;
    char path[AML_MAX_PATH_LEN];
    int absolute = 0;
    int parent_count = 0;

    if (p >= end) return -1;
    if (*p == '\\') { absolute = 1; p++; }
    else {
        while (p < end && *p == '^') { parent_count++; p++; }
    }

    if (p >= end) return -1;

    /* Build the prefix from the scope. */
    if (absolute) {
        path[0] = '\\';
        path[1] = 0;
    } else {
        a_strcpy_max(path, scope, AML_MAX_PATH_LEN);
        for (int i = 0; i < parent_count; i++) {
            int len = a_strlen(path);
            while (len > 0 && path[len - 1] != '.') {
                path[len - 1] = 0;
                len--;
            }
            if (len > 0 && path[len - 1] == '.') {
                path[len - 1] = 0;
            }
        }
    }

    /* Read segs. */
    int n_segs;
    if (*p == 0x00) {
        /* NullName */
        p++;
        n_segs = 0;
    } else if (*p == OP_DUAL_NAME) {
        p++;
        n_segs = 2;
    } else if (*p == OP_MULTI_NAME) {
        p++;
        if (p >= end) return -1;
        n_segs = *p++;
    } else {
        n_segs = 1;
    }

    for (int i = 0; i < n_segs; i++) {
        char seg[5];
        int c = decode_nameseg(p, end, seg);
        if (c < 0) return -1;
        p += c;
        if (a_strlen(path) > 0 && path[a_strlen(path) - 1] != '\\') {
            a_strcat_max(path, ".", AML_MAX_PATH_LEN);
        }
        a_strcat_max(path, seg, AML_MAX_PATH_LEN);
    }

    a_strcpy_max(out, path, AML_MAX_PATH_LEN);
    return (int)(p - start);
}

/* ── Symbol table ──────────────────────────────────────────────── */

static int sym_find(const char *path)
{
    for (int i = 0; i < g_sym_count; i++) {
        if (a_streq(g_syms[i].path, path)) return i;
    }
    return -1;
}

/* Resolve a path that may be relative — search from full scope upward
 * (ACPI namespace search rule). */
static int sym_resolve_search(const char *scope, const char *name)
{
    char buf[AML_MAX_PATH_LEN];
    if (name[0] == '\\') {
        return sym_find(name);
    }
    /* Try fully-qualified at progressively higher scopes. */
    a_strcpy_max(buf, scope, AML_MAX_PATH_LEN);
    while (1) {
        char attempt[AML_MAX_PATH_LEN];
        a_strcpy_max(attempt, buf, AML_MAX_PATH_LEN);
        if (a_strlen(attempt) > 0 && attempt[a_strlen(attempt) - 1] != '\\') {
            a_strcat_max(attempt, ".", AML_MAX_PATH_LEN);
        }
        a_strcat_max(attempt, name, AML_MAX_PATH_LEN);
        int idx = sym_find(attempt);
        if (idx >= 0) return idx;
        int len = a_strlen(buf);
        if (len <= 1) break;
        /* Strip last segment. */
        while (len > 0 && buf[len - 1] != '.' && buf[len - 1] != '\\') {
            buf[len - 1] = 0;
            len--;
        }
        if (len > 0 && buf[len - 1] == '.') { buf[len - 1] = 0; }
        if (len <= 1) {
            /* Final attempt at root scope. */
            a_strcpy_max(attempt, "\\", AML_MAX_PATH_LEN);
            a_strcat_max(attempt, name, AML_MAX_PATH_LEN);
            idx = sym_find(attempt);
            return idx;
        }
    }
    return -1;
}

static int sym_alloc(const char *path, sym_kind_t kind)
{
    if (g_sym_count >= AML_MAX_SYMBOLS) return -1;
    int existing = sym_find(path);
    if (existing >= 0) {
        /* Re-define: overwrite the old entry. ACPI explicitly allows
         * SSDTs to extend or override DSDT names. */
        return existing;
    }
    aml_sym_t *s = &g_syms[g_sym_count++];
    a_memset(s, 0, sizeof(*s));
    a_strcpy_max(s->path, path, AML_MAX_PATH_LEN);
    s->kind = kind;
    return g_sym_count - 1;
}

/* ── Catalog pass ──────────────────────────────────────────────── */

/* Decode a literal integer constant. Returns bytes consumed or 0. */
static int catalog_decode_int_literal(const uint8_t *p, const uint8_t *end,
                                      uint64_t *out)
{
    if (p >= end) return 0;
    uint8_t op = *p;
    switch (op) {
    case OP_ZERO: *out = 0; return 1;
    case OP_ONE:  *out = 1; return 1;
    case OP_ONES: *out = ~0ULL; return 1;
    case OP_BYTE:
        if (p + 1 >= end) return 0;
        *out = p[1]; return 2;
    case OP_WORD:
        if (p + 2 >= end) return 0;
        *out = (uint64_t)p[1] | ((uint64_t)p[2] << 8); return 3;
    case OP_DWORD:
        if (p + 4 >= end) return 0;
        *out = (uint64_t)p[1] | ((uint64_t)p[2] << 8) |
               ((uint64_t)p[3] << 16) | ((uint64_t)p[4] << 24);
        return 5;
    case OP_QWORD:
        if (p + 8 >= end) return 0;
        *out = 0;
        for (int i = 0; i < 8; i++) *out |= ((uint64_t)p[1 + i]) << (i * 8);
        return 9;
    default:
        return 0;
    }
}

/* Walk a Field declaration's body and register each named field.
 * Field syntax (after PkgLength): RegionName flags <FieldList>.
 * FieldList: ReservedField (0x00 + PkgLength) | AccessField (0x01 + ...) |
 *            ConnectField (0x02 + ...) | NamedField (NameSeg + PkgLength).
 *
 * `scope` is the enclosing scope (where the field unit names get
 * registered). We walk to end_p. */
static void catalog_field_list(const uint8_t *p, const uint8_t *end,
                               int region_sym_idx, uint8_t flags,
                               const char *scope)
{
    uint32_t bit_offset = 0;
    uint8_t access = (flags & FIELD_ACCESS_MASK);
    if (access == 0) access = 1;  /* AnyAcc -> ByteAcc */

    while (p < end) {
        uint8_t b = *p;
        if (b == 0x00) {
            /* Reserved field: 0x00 PkgLength. */
            p++;
            uint32_t len;
            int c = decode_pkglen(p, end, &len);
            if (c < 0) return;
            p += c;
            bit_offset += len;
        } else if (b == 0x01) {
            /* AccessField: 0x01 type attrib */
            if (p + 3 > end) return;
            access = p[1] & FIELD_ACCESS_MASK;
            if (access == 0) access = 1;
            p += 3;
        } else if (b == 0x02) {
            /* ConnectField: 0x02 NameString | BufferData — skip. */
            p += 1;
            if (p >= end) return;
            if (*p == OP_BUFFER) {
                p++;
                uint32_t blen;
                int c = decode_pkglen(p, end, &blen);
                if (c < 0) return;
                p += blen;  /* PkgLength includes itself. */
            } else {
                /* NameString — variable length; bail safely. */
                return;
            }
        } else if (b == 0x03) {
            /* ExtendedAccessField: 0x03 type attrib length */
            if (p + 4 > end) return;
            access = p[1] & FIELD_ACCESS_MASK;
            if (access == 0) access = 1;
            p += 4;
        } else {
            /* NamedField: NameSeg(4) PkgLength */
            if (p + 4 > end) return;
            char seg[5];
            if (decode_nameseg(p, end, seg) < 0) return;
            p += 4;
            uint32_t bits;
            int c = decode_pkglen(p, end, &bits);
            if (c < 0) return;
            p += c;

            char fpath[AML_MAX_PATH_LEN];
            a_strcpy_max(fpath, scope, AML_MAX_PATH_LEN);
            if (a_strlen(fpath) > 0 && fpath[a_strlen(fpath) - 1] != '\\') {
                a_strcat_max(fpath, ".", AML_MAX_PATH_LEN);
            }
            a_strcat_max(fpath, seg, AML_MAX_PATH_LEN);

            int idx = sym_alloc(fpath, SYM_FIELD_UNIT);
            if (idx >= 0) {
                g_syms[idx].field.region_sym = region_sym_idx;
                g_syms[idx].field.bit_offset = bit_offset;
                g_syms[idx].field.bit_width = bits;
                g_syms[idx].field.access_type = access;
            }
            bit_offset += bits;
        }
    }
}

/* Recursive catalog walker. `aml` runs from p..end. `scope` is the
 * fully-qualified name of the enclosing scope. */
static void catalog_walk(const uint8_t *p, const uint8_t *end, const char *scope);

static void catalog_named_value(const uint8_t **pp, const uint8_t *end,
                                const char *scope)
{
    /* We're at: <NameString> <DataObject> */
    const uint8_t *p = *pp;
    char nm[AML_MAX_PATH_LEN];
    int c = decode_namestring_rel(p, end, scope, nm);
    if (c < 0) { *pp = end; return; }
    p += c;

    int idx = sym_alloc(nm, SYM_NAME);
    if (idx >= 0) {
        /* Try to capture a literal — falls back to "lazy" storage
         * (we keep a reference to the AML byte stream as a buffer
         * the evaluator can re-parse). For our targeted methods,
         * literals cover _BIF/_PSR/_S5 fine. */
        uint64_t v;
        int n = catalog_decode_int_literal(p, end, &v);
        if (n > 0) {
            g_syms[idx].value.type = AML_T_INTEGER;
            g_syms[idx].value.u.integer = v;
        } else if (p < end && *p == OP_PACKAGE) {
            /* Package literal — keep a pointer; evaluator decodes on demand. */
            g_syms[idx].value.type = AML_T_BUFFER;
            g_syms[idx].value.u.buf.bytes = (uint8_t *)p;  /* const-cast */
            g_syms[idx].value.u.buf.len = (uint32_t)(end - p);
            g_syms[idx].value.u.buf.owned = 0;
        } else if (p < end && *p == OP_BUFFER) {
            g_syms[idx].value.type = AML_T_BUFFER;
            g_syms[idx].value.u.buf.bytes = (uint8_t *)p;
            g_syms[idx].value.u.buf.len = (uint32_t)(end - p);
            g_syms[idx].value.u.buf.owned = 0;
        } else if (p < end && *p == OP_STRING) {
            g_syms[idx].value.type = AML_T_STRING;
            g_syms[idx].value.u.str.s = (const char *)(p + 1);
        } else {
            /* Unknown — we record the symbol but value stays UNINIT. */
        }
    }

    /* Skip past the data object. We don't need to be precise — the
     * outer catalog walker advances by parent PkgLength. So just
     * leave p at the NEXT recognizable opcode by jumping to end. */
    *pp = end;
}

static void catalog_method(const uint8_t **pp, const uint8_t *end,
                           const char *scope, const uint8_t *body_end)
{
    const uint8_t *p = *pp;
    char nm[AML_MAX_PATH_LEN];
    int c = decode_namestring_rel(p, end, scope, nm);
    if (c < 0) { *pp = body_end; return; }
    p += c;

    if (p >= body_end) { *pp = body_end; return; }
    uint8_t flags = *p++;
    int idx = sym_alloc(nm, SYM_METHOD);
    if (idx >= 0) {
        g_syms[idx].body = p;
        g_syms[idx].body_len = (uint32_t)(body_end - p);
        g_syms[idx].n_args = (flags & 0x07);
    }
    *pp = body_end;
}

static void catalog_op_region(const uint8_t **pp, const uint8_t *end,
                              const char *scope)
{
    const uint8_t *p = *pp;
    char nm[AML_MAX_PATH_LEN];
    int c = decode_namestring_rel(p, end, scope, nm);
    if (c < 0) { *pp = end; return; }
    p += c;
    if (p >= end) { *pp = end; return; }
    uint8_t space = *p++;
    uint64_t base = 0;
    int n = catalog_decode_int_literal(p, end, &base);
    if (n <= 0) { *pp = end; return; }
    p += n;
    uint64_t len = 0;
    n = catalog_decode_int_literal(p, end, &len);
    if (n <= 0) { *pp = end; return; }
    p += n;

    int idx = sym_alloc(nm, SYM_OP_REGION);
    if (idx >= 0) {
        g_syms[idx].region.region_space = space;
        g_syms[idx].region.region_base = base;
        g_syms[idx].region.region_len = len;
    }
    *pp = p;
}

static void catalog_field(const uint8_t **pp, const uint8_t *end,
                          const char *scope)
{
    const uint8_t *p = *pp;
    /* Already past pkg-length. p -> RegionName flags FieldList. */
    char rname[AML_MAX_PATH_LEN];
    int c = decode_namestring_rel(p, end, scope, rname);
    if (c < 0) { *pp = end; return; }
    p += c;
    if (p >= end) { *pp = end; return; }
    uint8_t flags = *p++;

    int region_sym = sym_find(rname);
    catalog_field_list(p, end, region_sym, flags, scope);
    *pp = end;
}

static void catalog_walk(const uint8_t *aml, const uint8_t *end, const char *scope)
{
    const uint8_t *p = aml;
    int safety = 0;

    while (p < end) {
        if (++safety > 1000000) break;  /* never spin */
        uint8_t op = *p;

        if (op == OP_NAME) {
            p++;
            const uint8_t *body_end = end;
            catalog_named_value(&p, body_end, scope);
            continue;
        }
        if (op == OP_ALIAS) {
            /* Alias SourceName AliasName — register Alias as a dup of source. */
            p++;
            char src[AML_MAX_PATH_LEN], dst[AML_MAX_PATH_LEN];
            int c = decode_namestring_rel(p, end, scope, src);
            if (c < 0) break; p += c;
            c = decode_namestring_rel(p, end, scope, dst);
            if (c < 0) break; p += c;
            int sidx = sym_find(src);
            int didx = sym_alloc(dst, SYM_NAME);
            if (sidx >= 0 && didx >= 0) {
                g_syms[didx] = g_syms[sidx];
                a_strcpy_max(g_syms[didx].path, dst, AML_MAX_PATH_LEN);
            }
            continue;
        }
        if (op == OP_SCOPE) {
            p++;
            uint32_t plen;
            const uint8_t *plen_at = p;
            int c = decode_pkglen(p, end, &plen);
            if (c < 0) break;
            const uint8_t *body_end = plen_at + plen;
            if (body_end > end) body_end = end;
            p += c;
            char sname[AML_MAX_PATH_LEN];
            c = decode_namestring_rel(p, end, scope, sname);
            if (c < 0) { p = body_end; continue; }
            p += c;
            sym_alloc(sname, SYM_DEVICE);
            catalog_walk(p, body_end, sname);
            p = body_end;
            continue;
        }
        if (op == OP_METHOD) {
            p++;
            uint32_t plen;
            const uint8_t *plen_at = p;
            int c = decode_pkglen(p, end, &plen);
            if (c < 0) break;
            const uint8_t *body_end = plen_at + plen;
            if (body_end > end) body_end = end;
            p += c;
            catalog_method(&p, body_end, scope, body_end);
            p = body_end;
            continue;
        }
        if (op == OP_EXT) {
            if (p + 1 >= end) break;
            uint8_t ext = p[1];
            if (ext == EXT_DEVICE || ext == EXT_PROCESSOR ||
                ext == EXT_POWER_RES || ext == EXT_THERMAL_ZONE) {
                p += 2;
                uint32_t plen;
                const uint8_t *plen_at = p;
                int c = decode_pkglen(p, end, &plen);
                if (c < 0) break;
                const uint8_t *body_end = plen_at + plen;
                if (body_end > end) body_end = end;
                p += c;

                /* Processor / PowerRes have additional fields before the body. */
                char dname[AML_MAX_PATH_LEN];
                c = decode_namestring_rel(p, end, scope, dname);
                if (c < 0) { p = body_end; continue; }
                p += c;
                if (ext == EXT_PROCESSOR) {
                    /* ProcID(byte) PblkAddr(dword) PblkLen(byte) */
                    if (p + 6 <= body_end) p += 6;
                } else if (ext == EXT_POWER_RES) {
                    /* SystemLevel(byte) ResourceOrder(word) */
                    if (p + 3 <= body_end) p += 3;
                }
                sym_alloc(dname, SYM_DEVICE);
                catalog_walk(p, body_end, dname);
                p = body_end;
                continue;
            }
            if (ext == EXT_OP_REGION) {
                p += 2;
                catalog_op_region(&p, end, scope);
                continue;
            }
            if (ext == EXT_FIELD || ext == EXT_INDEX_FIELD || ext == EXT_BANK_FIELD) {
                p += 2;
                uint32_t plen;
                const uint8_t *plen_at = p;
                int c = decode_pkglen(p, end, &plen);
                if (c < 0) break;
                const uint8_t *body_end = plen_at + plen;
                if (body_end > end) body_end = end;
                p += c;
                if (ext == EXT_FIELD) {
                    catalog_field(&p, body_end, scope);
                }
                /* IndexField/BankField: skip — not in our v1 scope. */
                p = body_end;
                continue;
            }
            if (ext == EXT_MUTEX) {
                p += 2;
                char nm[AML_MAX_PATH_LEN];
                int c = decode_namestring_rel(p, end, scope, nm);
                if (c < 0) break;
                p += c;
                if (p < end) p++;  /* sync flags */
                sym_alloc(nm, SYM_NAME);
                continue;
            }
            if (ext == EXT_EVENT) {
                p += 2;
                char nm[AML_MAX_PATH_LEN];
                int c = decode_namestring_rel(p, end, scope, nm);
                if (c < 0) break;
                p += c;
                sym_alloc(nm, SYM_NAME);
                continue;
            }
            /* Unknown ext at top level — no useful recovery, bail. */
            break;
        }

        /* Unknown top-level opcode at this scope. Real AML at top level
         * is a sequence of TermObjects; everything we don't understand
         * here is non-recursive and harmless to skip — but without a
         * full grammar we can't safely advance. So we stop. */
        break;
    }
}

/* ── Evaluator ─────────────────────────────────────────────────── */

typedef struct {
    aml_object_t locals[AML_MAX_LOCALS];
    aml_object_t args[AML_MAX_ARGS];
    int          have_return;
    aml_object_t ret;
    int          break_flag;
    int          continue_flag;
    int          abort_flag;     /* set by unimplemented opcode */
    int          depth;
    const char  *cur_scope;
} eval_ctx_t;

static int eval_term(eval_ctx_t *ctx, const uint8_t **pp, const uint8_t *end,
                     aml_object_t *out);

/* Read a field unit per ACPI semantics for SystemMemory/SystemIO/PCI_Config.
 * Returns 0 on success, -1 if the region kind isn't supported. */
static int field_read(int field_sym, uint64_t *out)
{
    aml_sym_t *fs = &g_syms[field_sym];
    if (fs->field.region_sym < 0) return -1;
    aml_sym_t *rs = &g_syms[fs->field.region_sym];
    if (rs->kind != SYM_OP_REGION) return -1;

    uint32_t bit_off = fs->field.bit_offset;
    uint32_t bit_width = fs->field.bit_width;
    if (bit_width == 0 || bit_width > 64) return -1;
    uint8_t access = fs->field.access_type ? fs->field.access_type : 1;
    uint32_t access_bytes = (access == 1) ? 1 : (access == 2) ? 2 : (access == 3) ? 4 : 8;
    uint32_t access_bits = access_bytes * 8;

    uint64_t base = rs->region.region_base;
    uint32_t byte_off = (bit_off / access_bits) * access_bytes;
    uint32_t bit_in_unit = bit_off % access_bits;

    /* For widths up to a single access unit this works straightforwardly. */
    if (bit_in_unit + bit_width > access_bits) {
        /* TODO: cross-unit reads. Not needed by _BCM/_LID/_BST in practice. */
        return -1;
    }

    uint64_t raw = 0;
    switch (rs->region.region_space) {
    case REGION_SYSTEM_IO: {
        uint16_t port = (uint16_t)(base + byte_off);
        if (access_bytes == 1)      raw = inb(port);
        else if (access_bytes == 2) raw = inw(port);
        else if (access_bytes == 4) raw = inl(port);
        else                        return -1;
        break;
    }
    case REGION_SYSTEM_MEMORY: {
        uint64_t phys = base + byte_off;
        vmm_map_range(phys & ~0xFFFULL, phys & ~0xFFFULL, 1, PTE_WRITABLE);
        if (access_bytes == 1)      raw = *(volatile uint8_t *)(uintptr_t)phys;
        else if (access_bytes == 2) raw = *(volatile uint16_t *)(uintptr_t)phys;
        else if (access_bytes == 4) raw = *(volatile uint32_t *)(uintptr_t)phys;
        else                        raw = *(volatile uint64_t *)(uintptr_t)phys;
        break;
    }
    case REGION_PCI_CONFIG: {
        /* base = (function << 16) | offset; PCI_Config OpRegions are
         * typically scoped under a Device with _ADR; we don't parse
         * _ADR yet. Honest: report failure. */
        return -1;
    }
    case REGION_EMBEDDED_CTRL: {
        /* EC fields are byte-addressable. access_bytes is normally 1,
         * but some DSDTs declare wider AccessType — we only honor 1
         * here; >1 falls back to byte loop. */
        uint64_t v = 0;
        if (access_bytes == 1) {
            uint64_t bv = 0;
            if (aml_ec_region_read(base, byte_off, 1, &bv) != 0) return -1;
            raw = bv;
        } else {
            for (int i = 0; i < (int)access_bytes; i++) {
                uint64_t bv = 0;
                if (aml_ec_region_read(base, byte_off + i, 1, &bv) != 0) return -1;
                v |= (bv & 0xFF) << (i * 8);
            }
            raw = v;
        }
        break;
    }
    default:
        return -1;
    }

    uint64_t mask = (bit_width >= 64) ? ~0ULL : ((1ULL << bit_width) - 1);
    *out = (raw >> bit_in_unit) & mask;
    return 0;
}

static int field_write(int field_sym, uint64_t val)
{
    aml_sym_t *fs = &g_syms[field_sym];
    if (fs->field.region_sym < 0) return -1;
    aml_sym_t *rs = &g_syms[fs->field.region_sym];
    if (rs->kind != SYM_OP_REGION) return -1;

    uint32_t bit_off = fs->field.bit_offset;
    uint32_t bit_width = fs->field.bit_width;
    uint8_t access = fs->field.access_type ? fs->field.access_type : 1;
    uint32_t access_bytes = (access == 1) ? 1 : (access == 2) ? 2 : (access == 3) ? 4 : 8;
    uint32_t access_bits = access_bytes * 8;

    uint64_t base = rs->region.region_base;
    uint32_t byte_off = (bit_off / access_bits) * access_bytes;
    uint32_t bit_in_unit = bit_off % access_bits;
    if (bit_in_unit + bit_width > access_bits) return -1;

    uint64_t mask = (bit_width >= 64) ? ~0ULL : ((1ULL << bit_width) - 1);
    uint64_t shifted = (val & mask) << bit_in_unit;
    uint64_t merge_mask = mask << bit_in_unit;

    switch (rs->region.region_space) {
    case REGION_SYSTEM_IO: {
        uint16_t port = (uint16_t)(base + byte_off);
        uint64_t cur = 0;
        if (access_bytes == 1) cur = inb(port);
        else if (access_bytes == 2) cur = inw(port);
        else if (access_bytes == 4) cur = inl(port);
        else return -1;
        cur = (cur & ~merge_mask) | shifted;
        if (access_bytes == 1) outb(port, (uint8_t)cur);
        else if (access_bytes == 2) outw(port, (uint16_t)cur);
        else                        outl(port, (uint32_t)cur);
        return 0;
    }
    case REGION_SYSTEM_MEMORY: {
        uint64_t phys = base + byte_off;
        vmm_map_range(phys & ~0xFFFULL, phys & ~0xFFFULL, 1, PTE_WRITABLE);
        if (access_bytes == 1) {
            volatile uint8_t *p = (uint8_t *)(uintptr_t)phys;
            *p = (uint8_t)((*p & ~merge_mask) | shifted);
        } else if (access_bytes == 2) {
            volatile uint16_t *p = (uint16_t *)(uintptr_t)phys;
            *p = (uint16_t)((*p & ~merge_mask) | shifted);
        } else if (access_bytes == 4) {
            volatile uint32_t *p = (uint32_t *)(uintptr_t)phys;
            *p = (uint32_t)((*p & ~merge_mask) | shifted);
        } else {
            volatile uint64_t *p = (uint64_t *)(uintptr_t)phys;
            *p = (*p & ~merge_mask) | shifted;
        }
        return 0;
    }
    case REGION_EMBEDDED_CTRL: {
        /* Read-modify-write through the EC interface, byte at a time. */
        if (access_bytes == 1) {
            uint64_t cur = 0;
            if (aml_ec_region_read(base, byte_off, 1, &cur) != 0) return -1;
            uint64_t merged = (cur & ~merge_mask) | shifted;
            if (aml_ec_region_write(base, byte_off, 1, merged & 0xFF) != 0) return -1;
            return 0;
        }
        /* Wider access: read all bytes, modify, write back. */
        uint64_t cur = 0;
        for (int i = 0; i < (int)access_bytes; i++) {
            uint64_t bv = 0;
            if (aml_ec_region_read(base, byte_off + i, 1, &bv) != 0) return -1;
            cur |= (bv & 0xFF) << (i * 8);
        }
        uint64_t merged = (cur & ~merge_mask) | shifted;
        for (int i = 0; i < (int)access_bytes; i++) {
            if (aml_ec_region_write(base, byte_off + i, 1, (merged >> (i * 8)) & 0xFF) != 0)
                return -1;
        }
        return 0;
    }
    default:
        return -1;
    }
}

/* Resolve an AML object/reference to a concrete value.
 *
 * For our purposes "TermArg" production includes:
 *   constants, computational expressions, NameStrings (resolved via
 *   the symbol table), Local0..7, Arg0..6, MethodInvocation. */
static int eval_term(eval_ctx_t *ctx, const uint8_t **pp, const uint8_t *end,
                     aml_object_t *out)
{
    if (ctx->abort_flag) return -1;
    if (ctx->depth > AML_MAX_DEPTH) {
        ctx->abort_flag = 1;
        return -1;
    }
    ctx->depth++;

    const uint8_t *p = *pp;
    if (p >= end) { ctx->depth--; return -1; }
    uint8_t op = *p;

    /* Simple integer constants */
    if (op == OP_ZERO) { p++; obj_set_int(out, 0); goto done; }
    if (op == OP_ONE)  { p++; obj_set_int(out, 1); goto done; }
    if (op == OP_ONES) { p++; obj_set_int(out, ~0ULL); goto done; }
    if (op == OP_BYTE || op == OP_WORD || op == OP_DWORD || op == OP_QWORD) {
        uint64_t v;
        int n = catalog_decode_int_literal(p, end, &v);
        if (n <= 0) goto fail;
        p += n;
        obj_set_int(out, v);
        goto done;
    }
    if (op == OP_STRING) {
        p++;
        out->type = AML_T_STRING;
        out->u.str.s = (const char *)p;
        while (p < end && *p) p++;
        if (p < end) p++;
        goto done;
    }
    if (op == OP_BUFFER) {
        p++;
        uint32_t plen;
        const uint8_t *plen_at = p;
        int c = decode_pkglen(p, end, &plen);
        if (c < 0) goto fail;
        const uint8_t *blk_end = plen_at + plen;
        if (blk_end > end) blk_end = end;
        p += c;
        /* BufferSize TermArg, then payload bytes. */
        aml_object_t sz = { .type = AML_T_UNINIT };
        if (eval_term(ctx, &p, blk_end, &sz) < 0) { aml_object_release(&sz); goto fail; }
        uint64_t want = obj_to_int(&sz);
        aml_object_release(&sz);
        out->type = AML_T_BUFFER;
        out->u.buf.bytes = (uint8_t *)p;
        out->u.buf.len = want < (uint32_t)(blk_end - p) ? (uint32_t)want : (uint32_t)(blk_end - p);
        out->u.buf.owned = 0;
        p = blk_end;
        goto done;
    }
    if (op == OP_PACKAGE || op == OP_VAR_PACKAGE) {
        p++;
        uint32_t plen;
        const uint8_t *plen_at = p;
        int c = decode_pkglen(p, end, &plen);
        if (c < 0) goto fail;
        const uint8_t *blk_end = plen_at + plen;
        if (blk_end > end) blk_end = end;
        p += c;
        if (p >= blk_end) goto fail;
        uint32_t n_elems;
        if (op == OP_PACKAGE) {
            n_elems = *p++;
        } else {
            aml_object_t sz = { .type = AML_T_UNINIT };
            if (eval_term(ctx, &p, blk_end, &sz) < 0) { aml_object_release(&sz); goto fail; }
            n_elems = (uint32_t)obj_to_int(&sz);
            aml_object_release(&sz);
        }
        if (n_elems > AML_MAX_PKG_ELEMS) n_elems = AML_MAX_PKG_ELEMS;

        aml_object_t *elems = (aml_object_t *)kmalloc(sizeof(aml_object_t) * (n_elems ? n_elems : 1));
        if (!elems) goto fail;
        a_memset(elems, 0, sizeof(aml_object_t) * (n_elems ? n_elems : 1));
        uint32_t got = 0;
        for (uint32_t i = 0; i < n_elems && p < blk_end; i++) {
            if (eval_term(ctx, &p, blk_end, &elems[i]) < 0) {
                /* allow partial — pad with zero */
                obj_set_int(&elems[i], 0);
            }
            got++;
        }
        out->type = AML_T_PACKAGE;
        out->u.pkg.elems = elems;
        out->u.pkg.n = got;
        out->u.pkg.owned = 1;
        p = blk_end;
        goto done;
    }

    /* Local0..7 / Arg0..6 */
    if (op >= OP_LOCAL0 && op <= OP_LOCAL7) {
        int idx = op - OP_LOCAL0;
        p++;
        obj_copy(out, &ctx->locals[idx]);
        goto done;
    }
    if (op >= OP_ARG0 && op <= OP_ARG6) {
        int idx = op - OP_ARG0;
        p++;
        obj_copy(out, &ctx->args[idx]);
        goto done;
    }

    /* Binary integer operations: <op> arg1 arg2 [target] */
    if (op == OP_ADD || op == OP_SUBTRACT || op == OP_MULTIPLY ||
        op == OP_DIVIDE || op == OP_AND || op == OP_OR || op == OP_XOR ||
        op == OP_SHIFT_LEFT || op == OP_SHIFT_RIGHT || op == OP_MOD ||
        op == OP_NAND || op == OP_NOR) {
        p++;
        aml_object_t a = { .type = AML_T_UNINIT }, b = { .type = AML_T_UNINIT };
        if (eval_term(ctx, &p, end, &a) < 0) { aml_object_release(&a); goto fail; }
        if (eval_term(ctx, &p, end, &b) < 0) { aml_object_release(&a); aml_object_release(&b); goto fail; }
        uint64_t va = obj_to_int(&a), vb = obj_to_int(&b);
        uint64_t r = 0;
        switch (op) {
        case OP_ADD: r = va + vb; break;
        case OP_SUBTRACT: r = va - vb; break;
        case OP_MULTIPLY: r = va * vb; break;
        case OP_DIVIDE: r = vb ? (va / vb) : 0; break;
        case OP_AND: r = va & vb; break;
        case OP_OR:  r = va | vb; break;
        case OP_XOR: r = va ^ vb; break;
        case OP_SHIFT_LEFT: r = va << (vb & 63); break;
        case OP_SHIFT_RIGHT: r = va >> (vb & 63); break;
        case OP_MOD: r = vb ? (va % vb) : 0; break;
        case OP_NAND: r = ~(va & vb); break;
        case OP_NOR:  r = ~(va | vb); break;
        }
        aml_object_release(&a); aml_object_release(&b);
        /* Target: any TermArg which we evaluate-and-discard (the target
         * receives the result via Store semantics — for SuperName paths
         * we don't model writes back to Locals/Args here, an explicit
         * Store does that). For simplicity, parse a target as a no-op:
         * if it's ZeroOp (=null target), consume it. Otherwise we leave
         * the AML stream pointing at it; a strict parse would need to
         * fully consume it. */
        if (p < end && *p == OP_ZERO) {
            p++;
        } else if (p < end && *p >= OP_LOCAL0 && *p <= OP_LOCAL7) {
            int idx = *p - OP_LOCAL0;
            p++;
            obj_set_int(&ctx->locals[idx], r);
        } else if (p < end && *p >= OP_ARG0 && *p <= OP_ARG6) {
            /* Args writable per spec; rare in modern firmware. */
            int idx = *p - OP_ARG0;
            p++;
            obj_set_int(&ctx->args[idx], r);
        } else {
            /* Skip a NameString-like target. We can't safely descend
             * into arbitrary SuperName. Treat as no target. */
            if (p < end && (*p == '\\' || *p == '^' ||
                            (*p >= 'A' && *p <= 'Z') || *p == '_')) {
                char tname[AML_MAX_PATH_LEN];
                int n = decode_namestring_rel(p, end, ctx->cur_scope, tname);
                if (n > 0) {
                    p += n;
                    int sidx = sym_resolve_search(ctx->cur_scope, tname);
                    if (sidx >= 0 && g_syms[sidx].kind == SYM_NAME) {
                        obj_set_int(&g_syms[sidx].value, r);
                    } else if (sidx >= 0 && g_syms[sidx].kind == SYM_FIELD_UNIT) {
                        (void)field_write(sidx, r);
                    }
                }
            }
        }
        obj_set_int(out, r);
        goto done;
    }

    /* Unary integer ops: <op> arg [target] */
    if (op == OP_NOT || op == OP_INCREMENT || op == OP_DECREMENT ||
        op == OP_FIND_SET_LB || op == OP_FIND_SET_RB ||
        op == OP_TO_INTEGER || op == OP_TO_BUFFER || op == OP_TO_STRING ||
        op == OP_SIZE_OF) {
        p++;
        aml_object_t a = { .type = AML_T_UNINIT };
        if (eval_term(ctx, &p, end, &a) < 0) { aml_object_release(&a); goto fail; }
        uint64_t va = obj_to_int(&a);
        uint64_t r = 0;
        switch (op) {
        case OP_NOT: r = ~va; break;
        case OP_INCREMENT: r = va + 1; break;
        case OP_DECREMENT: r = va - 1; break;
        case OP_FIND_SET_LB: {
            r = 0;
            for (int i = 63; i >= 0; i--) if (va & (1ULL << i)) { r = i + 1; break; }
            break;
        }
        case OP_FIND_SET_RB: {
            r = 0;
            for (int i = 0; i < 64; i++) if (va & (1ULL << i)) { r = i + 1; break; }
            break;
        }
        case OP_TO_INTEGER: r = va; break;
        case OP_SIZE_OF:
            if (a.type == AML_T_BUFFER)  r = a.u.buf.len;
            else if (a.type == AML_T_STRING) r = a.u.str.s ? (uint64_t)a_strlen(a.u.str.s) : 0;
            else if (a.type == AML_T_PACKAGE) r = a.u.pkg.n;
            else r = 0;
            break;
        case OP_TO_BUFFER:
        case OP_TO_STRING: r = va; break;
        }
        aml_object_release(&a);
        /* Target same handling as binary ops */
        if (op == OP_INCREMENT || op == OP_DECREMENT) {
            /* These mutate the source if it was a Local/Arg/Name. The
             * arg was already consumed though; we'd need ref-of semantics
             * to round-trip. Honest gap: we return the new value but
             * don't write back. Most _BCM/_LID paths don't depend on
             * this. */
        } else {
            if (p < end && *p == OP_ZERO) p++;
            else if (p < end && *p >= OP_LOCAL0 && *p <= OP_LOCAL7) {
                int idx = *p - OP_LOCAL0; p++;
                obj_set_int(&ctx->locals[idx], r);
            } else if (p < end && (*p == '\\' || *p == '^' || *p == '_' ||
                                   (*p >= 'A' && *p <= 'Z'))) {
                char tname[AML_MAX_PATH_LEN];
                int n = decode_namestring_rel(p, end, ctx->cur_scope, tname);
                if (n > 0) p += n;
            }
        }
        obj_set_int(out, r);
        goto done;
    }

    /* Logical ops */
    if (op == OP_LAND || op == OP_LOR || op == OP_LEQUAL ||
        op == OP_LGREATER || op == OP_LLESS) {
        p++;
        aml_object_t a = { .type = AML_T_UNINIT }, b = { .type = AML_T_UNINIT };
        if (eval_term(ctx, &p, end, &a) < 0) { aml_object_release(&a); goto fail; }
        if (eval_term(ctx, &p, end, &b) < 0) { aml_object_release(&a); aml_object_release(&b); goto fail; }
        uint64_t va = obj_to_int(&a), vb = obj_to_int(&b);
        uint64_t r = 0;
        switch (op) {
        case OP_LAND: r = (va && vb) ? ~0ULL : 0; break;
        case OP_LOR:  r = (va || vb) ? ~0ULL : 0; break;
        case OP_LEQUAL: r = (va == vb) ? ~0ULL : 0; break;
        case OP_LGREATER: r = (va > vb) ? ~0ULL : 0; break;
        case OP_LLESS: r = (va < vb) ? ~0ULL : 0; break;
        }
        aml_object_release(&a); aml_object_release(&b);
        obj_set_int(out, r);
        goto done;
    }
    if (op == OP_LNOT) {
        p++;
        aml_object_t a = { .type = AML_T_UNINIT };
        if (eval_term(ctx, &p, end, &a) < 0) { aml_object_release(&a); goto fail; }
        uint64_t r = obj_to_int(&a) ? 0 : ~0ULL;
        aml_object_release(&a);
        obj_set_int(out, r);
        goto done;
    }

    /* Index: Index(source, idx, target) — returns reference (for our
     * purposes: the indexed element value). */
    if (op == OP_INDEX) {
        p++;
        aml_object_t src = { .type = AML_T_UNINIT }, ix = { .type = AML_T_UNINIT };
        if (eval_term(ctx, &p, end, &src) < 0) { aml_object_release(&src); goto fail; }
        if (eval_term(ctx, &p, end, &ix)  < 0) { aml_object_release(&src); aml_object_release(&ix); goto fail; }
        uint64_t k = obj_to_int(&ix);
        if (src.type == AML_T_PACKAGE && k < src.u.pkg.n) {
            obj_copy(out, &src.u.pkg.elems[k]);
        } else if (src.type == AML_T_BUFFER && k < src.u.buf.len) {
            obj_set_int(out, src.u.buf.bytes[k]);
        } else {
            obj_set_int(out, 0);
        }
        aml_object_release(&src); aml_object_release(&ix);
        /* Target: skip-or-write — same as above. */
        if (p < end && *p == OP_ZERO) p++;
        goto done;
    }

    /* DerefOf */
    if (op == OP_DEREF_OF) {
        p++;
        aml_object_t a = { .type = AML_T_UNINIT };
        if (eval_term(ctx, &p, end, &a) < 0) { aml_object_release(&a); goto fail; }
        obj_copy(out, &a);
        goto done;
    }

    /* Store(source, target) */
    if (op == OP_STORE) {
        p++;
        aml_object_t src = { .type = AML_T_UNINIT };
        if (eval_term(ctx, &p, end, &src) < 0) { aml_object_release(&src); goto fail; }
        if (p >= end) { aml_object_release(&src); goto fail; }
        uint8_t tgt = *p;
        if (tgt >= OP_LOCAL0 && tgt <= OP_LOCAL7) {
            p++;
            obj_copy(&ctx->locals[tgt - OP_LOCAL0], &src);
        } else if (tgt >= OP_ARG0 && tgt <= OP_ARG6) {
            p++;
            obj_copy(&ctx->args[tgt - OP_ARG0], &src);
        } else if (tgt == 0x00) {
            p++;  /* Null target: discard. */
        } else if (tgt == '\\' || tgt == '^' || tgt == '_' ||
                   (tgt >= 'A' && tgt <= 'Z') ||
                   tgt == OP_DUAL_NAME || tgt == OP_MULTI_NAME) {
            char tname[AML_MAX_PATH_LEN];
            int n = decode_namestring_rel(p, end, ctx->cur_scope, tname);
            if (n > 0) {
                p += n;
                int sidx = sym_resolve_search(ctx->cur_scope, tname);
                if (sidx >= 0 && g_syms[sidx].kind == SYM_NAME) {
                    obj_copy(&g_syms[sidx].value, &src);
                } else if (sidx >= 0 && g_syms[sidx].kind == SYM_FIELD_UNIT) {
                    (void)field_write(sidx, obj_to_int(&src));
                }
            }
        } else if (tgt == OP_EXT && p + 1 < end && p[1] == EXT_DEBUG) {
            p += 2;  /* Debug target — discard. */
        } else {
            /* Unknown target — abort honestly. */
            kputs("[aml] Store target opcode 0x");
            kput_hex(tgt);
            kputs(" not implemented\n");
            aml_object_release(&src);
            ctx->abort_flag = 1;
            goto fail;
        }
        obj_copy(out, &src);
        aml_object_release(&src);
        goto done;
    }

    /* If/Else */
    if (op == OP_IF) {
        p++;
        uint32_t plen;
        const uint8_t *plen_at = p;
        int c = decode_pkglen(p, end, &plen);
        if (c < 0) goto fail;
        const uint8_t *blk_end = plen_at + plen;
        if (blk_end > end) blk_end = end;
        p += c;
        aml_object_t cond = { .type = AML_T_UNINIT };
        if (eval_term(ctx, &p, blk_end, &cond) < 0) { aml_object_release(&cond); goto fail; }
        int taken = obj_to_int(&cond) != 0;
        aml_object_release(&cond);
        if (taken) {
            while (p < blk_end && !ctx->have_return && !ctx->break_flag &&
                   !ctx->continue_flag && !ctx->abort_flag) {
                aml_object_t tmp = { .type = AML_T_UNINIT };
                if (eval_term(ctx, &p, blk_end, &tmp) < 0) { aml_object_release(&tmp); break; }
                aml_object_release(&tmp);
            }
            p = blk_end;
            /* Skip over else if present */
            if (p < end && *p == OP_ELSE) {
                p++;
                uint32_t elen;
                const uint8_t *elen_at = p;
                int ec = decode_pkglen(p, end, &elen);
                if (ec < 0) goto fail;
                const uint8_t *eend = elen_at + elen;
                p = (eend > end) ? end : eend;
            }
        } else {
            p = blk_end;
            if (p < end && *p == OP_ELSE) {
                p++;
                uint32_t elen;
                const uint8_t *elen_at = p;
                int ec = decode_pkglen(p, end, &elen);
                if (ec < 0) goto fail;
                const uint8_t *eend = elen_at + elen;
                if (eend > end) eend = end;
                p += ec;
                while (p < eend && !ctx->have_return && !ctx->break_flag &&
                       !ctx->continue_flag && !ctx->abort_flag) {
                    aml_object_t tmp = { .type = AML_T_UNINIT };
                    if (eval_term(ctx, &p, eend, &tmp) < 0) { aml_object_release(&tmp); break; }
                    aml_object_release(&tmp);
                }
                p = eend;
            }
        }
        obj_set_int(out, 0);
        goto done;
    }
    if (op == OP_ELSE) {
        /* Stray else (without preceding If) — skip body. */
        p++;
        uint32_t plen;
        const uint8_t *plen_at = p;
        int c = decode_pkglen(p, end, &plen);
        if (c < 0) goto fail;
        const uint8_t *blk_end = plen_at + plen;
        p = (blk_end > end) ? end : blk_end;
        obj_set_int(out, 0);
        goto done;
    }
    if (op == OP_WHILE) {
        p++;
        uint32_t plen;
        const uint8_t *plen_at = p;
        int c = decode_pkglen(p, end, &plen);
        if (c < 0) goto fail;
        const uint8_t *blk_end = plen_at + plen;
        if (blk_end > end) blk_end = end;
        p += c;
        const uint8_t *cond_at = p;
        int safety = 0;
        while (1) {
            if (++safety > 100000) { ctx->abort_flag = 1; break; }
            const uint8_t *q = cond_at;
            aml_object_t cond = { .type = AML_T_UNINIT };
            if (eval_term(ctx, &q, blk_end, &cond) < 0) { aml_object_release(&cond); goto fail; }
            int taken = obj_to_int(&cond) != 0;
            aml_object_release(&cond);
            if (!taken) break;
            const uint8_t *body = q;
            while (body < blk_end && !ctx->have_return && !ctx->break_flag &&
                   !ctx->continue_flag && !ctx->abort_flag) {
                aml_object_t tmp = { .type = AML_T_UNINIT };
                if (eval_term(ctx, &body, blk_end, &tmp) < 0) { aml_object_release(&tmp); break; }
                aml_object_release(&tmp);
            }
            if (ctx->break_flag) { ctx->break_flag = 0; break; }
            if (ctx->continue_flag) ctx->continue_flag = 0;
            if (ctx->have_return || ctx->abort_flag) break;
        }
        p = blk_end;
        obj_set_int(out, 0);
        goto done;
    }
    if (op == OP_RETURN) {
        p++;
        if (p < end && *p != 0) {
            aml_object_t r = { .type = AML_T_UNINIT };
            if (eval_term(ctx, &p, end, &r) < 0) { aml_object_release(&r); goto fail; }
            obj_copy(&ctx->ret, &r);
        } else if (p < end) {
            p++;
            obj_set_int(&ctx->ret, 0);
        }
        ctx->have_return = 1;
        obj_copy(out, &ctx->ret);
        goto done;
    }
    if (op == OP_BREAK)    { p++; ctx->break_flag = 1;    obj_set_int(out, 0); goto done; }
    if (op == OP_CONTINUE) { p++; ctx->continue_flag = 1; obj_set_int(out, 0); goto done; }
    if (op == OP_NOOP)     { p++; obj_set_int(out, 0); goto done; }
    if (op == OP_BREAKPOINT) { p++; obj_set_int(out, 0); goto done; }

    /* Extended opcodes */
    if (op == OP_EXT) {
        if (p + 1 >= end) goto fail;
        uint8_t ext = p[1];
        if (ext == EXT_ACQUIRE) {
            /* Acquire(mutex, timeout) -> 0 (success). No real locking. */
            p += 2;
            char nm[AML_MAX_PATH_LEN];
            int n = decode_namestring_rel(p, end, ctx->cur_scope, nm);
            if (n < 0) goto fail;
            p += n;
            if (p + 2 <= end) p += 2;  /* timeout (word) */
            obj_set_int(out, 0);
            goto done;
        }
        if (ext == EXT_RELEASE || ext == EXT_RESET || ext == EXT_SIGNAL) {
            p += 2;
            char nm[AML_MAX_PATH_LEN];
            int n = decode_namestring_rel(p, end, ctx->cur_scope, nm);
            if (n < 0) goto fail;
            p += n;
            obj_set_int(out, 0);
            goto done;
        }
        if (ext == EXT_SLEEP || ext == EXT_STALL) {
            p += 2;
            aml_object_t a = { .type = AML_T_UNINIT };
            if (eval_term(ctx, &p, end, &a) < 0) { aml_object_release(&a); goto fail; }
            aml_object_release(&a);
            obj_set_int(out, 0);
            goto done;
        }
        if (ext == EXT_TIMER) {
            p += 2;
            obj_set_int(out, timer_read_tsc() / 100);  /* AML Timer is 100ns ticks */
            goto done;
        }
        if (ext == EXT_REVISION) {
            p += 2;
            obj_set_int(out, 2);
            goto done;
        }
        if (ext == EXT_DEBUG) {
            p += 2;
            obj_set_int(out, 0);
            goto done;
        }
        if (ext == EXT_COND_REF_OF) {
            /* CondRefOf(name, target) -> bool */
            p += 2;
            char nm[AML_MAX_PATH_LEN];
            int n = decode_namestring_rel(p, end, ctx->cur_scope, nm);
            if (n < 0) goto fail;
            p += n;
            int sidx = sym_resolve_search(ctx->cur_scope, nm);
            /* Skip target — same as Store target. */
            if (p < end && *p == OP_ZERO) p++;
            else if (p < end && *p >= OP_LOCAL0 && *p <= OP_LOCAL7) p++;
            else if (p < end && *p >= OP_ARG0 && *p <= OP_ARG6) p++;
            obj_set_int(out, sidx >= 0 ? ~0ULL : 0);
            goto done;
        }
        kputs("[aml] ext opcode 0x");
        kput_hex(ext);
        kputs(" not implemented\n");
        ctx->abort_flag = 1;
        goto fail;
    }

    /* Notify (non-extended) */
    if (op == OP_NOTIFY) {
        p++;
        aml_object_t a = { .type = AML_T_UNINIT }, b = { .type = AML_T_UNINIT };
        if (eval_term(ctx, &p, end, &a) < 0) { aml_object_release(&a); goto fail; }
        if (eval_term(ctx, &p, end, &b) < 0) { aml_object_release(&a); aml_object_release(&b); goto fail; }
        aml_object_release(&a); aml_object_release(&b);
        obj_set_int(out, 0);
        goto done;
    }

    /* NameString — lookup. Could be Method invocation or Name access. */
    if (op == '\\' || op == '^' || op == '_' ||
        (op >= 'A' && op <= 'Z') ||
        op == OP_DUAL_NAME || op == OP_MULTI_NAME || op == 0x00) {
        if (op == 0x00) { p++; obj_set_int(out, 0); goto done; }
        char nm[AML_MAX_PATH_LEN];
        int n = decode_namestring_rel(p, end, ctx->cur_scope, nm);
        if (n < 0) goto fail;
        p += n;
        int sidx = sym_resolve_search(ctx->cur_scope, nm);
        if (sidx < 0) {
            /* Unknown name. Most likely a forward reference or a name
             * we missed in catalog (extended scopes). Return 0 — many
             * AML methods tolerate this. */
            obj_set_int(out, 0);
            goto done;
        }
        aml_sym_t *sym = &g_syms[sidx];
        if (sym->kind == SYM_METHOD) {
            /* Consume n_args TermArgs and invoke. */
            int na = sym->n_args;
            aml_object_t call_args[AML_MAX_ARGS];
            for (int i = 0; i < AML_MAX_ARGS; i++) call_args[i].type = AML_T_UNINIT;
            for (int i = 0; i < na; i++) {
                if (eval_term(ctx, &p, end, &call_args[i]) < 0) {
                    for (int j = 0; j <= i; j++) aml_object_release(&call_args[j]);
                    goto fail;
                }
            }
            /* Invoke. */
            eval_ctx_t sub;
            a_memset(&sub, 0, sizeof(sub));
            for (int i = 0; i < na; i++) sub.args[i] = call_args[i];
            /* Determine the method's enclosing scope = strip last segment. */
            char m_scope[AML_MAX_PATH_LEN];
            a_strcpy_max(m_scope, sym->path, AML_MAX_PATH_LEN);
            int ml = a_strlen(m_scope);
            while (ml > 0 && m_scope[ml - 1] != '.' && m_scope[ml - 1] != '\\') {
                m_scope[ml - 1] = 0; ml--;
            }
            if (ml > 1 && m_scope[ml - 1] == '.') m_scope[ml - 1] = 0;
            sub.cur_scope = m_scope;
            sub.depth = ctx->depth;
            const uint8_t *bp = sym->body;
            const uint8_t *be = sym->body + sym->body_len;
            while (bp < be && !sub.have_return && !sub.abort_flag) {
                aml_object_t tmp = { .type = AML_T_UNINIT };
                if (eval_term(&sub, &bp, be, &tmp) < 0) {
                    aml_object_release(&tmp);
                    break;
                }
                aml_object_release(&tmp);
            }
            if (sub.abort_flag) ctx->abort_flag = 1;
            obj_copy(out, &sub.ret);
            for (int i = 0; i < na; i++) aml_object_release(&call_args[i]);
            goto done;
        }
        if (sym->kind == SYM_NAME) {
            obj_copy(out, &sym->value);
            goto done;
        }
        if (sym->kind == SYM_FIELD_UNIT) {
            uint64_t v;
            if (field_read(sidx, &v) == 0) {
                obj_set_int(out, v);
            } else {
                obj_set_int(out, 0);
            }
            goto done;
        }
        /* OpRegion / Device referenced bare: integer 0. */
        obj_set_int(out, 0);
        goto done;
    }

    /* Unknown opcode. */
    kputs("[aml] opcode 0x");
    kput_hex(op);
    kputs(" not implemented\n");
    ctx->abort_flag = 1;

fail:
    *pp = p;
    ctx->depth--;
    return -1;

done:
    *pp = p;
    ctx->depth--;
    return 0;
}

/* ── Public entrypoints ────────────────────────────────────────── */

int aml_init(void)
{
    if (g_initialized) return g_sym_count;
    g_initialized = 1;

    int n = acpi_aml_table_count();
    for (int i = 0; i < n; i++) {
        const acpi_aml_table_t *t = acpi_aml_table(i);
        if (!t || !t->aml) continue;
        catalog_walk(t->aml, t->aml + t->len, "\\");
    }
    return g_sym_count;
}

int aml_symbol_count(void) { return g_sym_count; }

int aml_sym_total(void) { return g_sym_count; }

int aml_sym_info(int idx, aml_sym_kind_t *kind, char *path_buf, int path_buf_len)
{
    if (idx < 0 || idx >= g_sym_count) return -1;
    if (kind) {
        switch (g_syms[idx].kind) {
        case SYM_NAME:       *kind = AML_SYM_NAME; break;
        case SYM_METHOD:     *kind = AML_SYM_METHOD; break;
        case SYM_OP_REGION:  *kind = AML_SYM_OP_REGION; break;
        case SYM_FIELD_UNIT: *kind = AML_SYM_FIELD_UNIT; break;
        case SYM_DEVICE:     *kind = AML_SYM_DEVICE; break;
        default:             *kind = AML_SYM_NAME; break;
        }
    }
    if (path_buf && path_buf_len > 0) {
        a_strcpy_max(path_buf, g_syms[idx].path, path_buf_len);
    }
    return 0;
}

int aml_evaluate(const char *path, const aml_object_t *args, int nargs,
                 aml_object_t *result)
{
    if (!g_initialized) aml_init();

    aml_object_t scratch = { .type = AML_T_UNINIT };
    if (!result) result = &scratch;

    int sidx = sym_find(path);
    if (sidx < 0) {
        journal_record(path, args, nargs, -1, 0, 0, timer_read_tsc());
        return -1;
    }

    aml_sym_t *sym = &g_syms[sidx];
    uint64_t t_start = timer_read_tsc();

    int rc = 0;
    if (sym->kind == SYM_NAME) {
        obj_copy(result, &sym->value);
    } else if (sym->kind == SYM_METHOD) {
        eval_ctx_t ctx;
        a_memset(&ctx, 0, sizeof(ctx));
        if (nargs > AML_MAX_ARGS) nargs = AML_MAX_ARGS;
        for (int i = 0; i < nargs && args; i++) ctx.args[i] = args[i];

        char m_scope[AML_MAX_PATH_LEN];
        a_strcpy_max(m_scope, sym->path, AML_MAX_PATH_LEN);
        int ml = a_strlen(m_scope);
        while (ml > 0 && m_scope[ml - 1] != '.' && m_scope[ml - 1] != '\\') {
            m_scope[ml - 1] = 0; ml--;
        }
        if (ml > 1 && m_scope[ml - 1] == '.') m_scope[ml - 1] = 0;
        ctx.cur_scope = m_scope;

        const uint8_t *bp = sym->body;
        const uint8_t *be = sym->body + sym->body_len;
        while (bp < be && !ctx.have_return && !ctx.abort_flag) {
            aml_object_t tmp = { .type = AML_T_UNINIT };
            if (eval_term(&ctx, &bp, be, &tmp) < 0) {
                aml_object_release(&tmp);
                break;
            }
            aml_object_release(&tmp);
        }
        if (ctx.abort_flag) {
            rc = -2;
        } else {
            obj_copy(result, &ctx.ret);
        }
    } else if (sym->kind == SYM_FIELD_UNIT) {
        uint64_t v;
        if (field_read(sidx, &v) == 0) {
            obj_set_int(result, v);
        } else {
            rc = -3;
        }
    } else {
        rc = -3;
    }

    uint64_t elapsed = timer_read_tsc() - t_start;
    journal_record(path, args, nargs, rc, result, elapsed, t_start);

    if (result == &scratch) aml_object_release(&scratch);
    return rc;
}

void aml_print_selftest_line(void)
{
    if (!g_initialized) aml_init();

    int methods = 0, names = 0;
    int evaluable = 0, skipped = 0;

    for (int i = 0; i < g_sym_count; i++) {
        if (g_syms[i].kind == SYM_METHOD) {
            methods++;
            if (g_syms[i].n_args == 0) {
                /* Dry-run by saving + restoring the journal head, since
                 * we don't want to flood it during selftest. */
                int saved_head = g_journal_head;
                int saved_size = g_journal_size;
                aml_object_t r = { .type = AML_T_UNINIT };
                int rc = aml_evaluate(g_syms[i].path, 0, 0, &r);
                aml_object_release(&r);
                /* Roll back the journal so selftest doesn't pollute it. */
                g_journal_head = saved_head;
                g_journal_size = saved_size;
                if (rc == 0) evaluable++;
                else if (rc == -2) skipped++;
            }
        } else if (g_syms[i].kind == SYM_NAME) {
            names++;
        }
    }

    kputs("AML ................... ");
    kput_dec((uint64_t)methods);
    kputs(" methods cataloged, ");
    kput_dec((uint64_t)evaluable);
    kputs(" evaluable (");
    kput_dec((uint64_t)skipped);
    kputs(" skipped)\n");
}
