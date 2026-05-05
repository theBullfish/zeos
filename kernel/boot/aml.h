/*
 * Zeos — minimum-viable AML (ACPI Machine Language) interpreter.
 *
 * Public API for evaluating AML methods + named values. Implementation
 * is in aml.c; see header comment there for the honest scope statement.
 */

#ifndef ZEOS_AML_H
#define ZEOS_AML_H

#include <stdint.h>

/* ── Tagged-union object ─────────────────────────────────────────── */

typedef enum {
    AML_T_UNINIT = 0,
    AML_T_INTEGER,
    AML_T_STRING,
    AML_T_BUFFER,
    AML_T_PACKAGE,
    AML_T_REFERENCE,   /* points back into the Name table */
} aml_type_t;

struct aml_object;
typedef struct aml_object aml_object_t;

struct aml_object {
    aml_type_t type;
    union {
        uint64_t        integer;
        struct {
            const char *s;     /* NUL-terminated, owned by interpreter */
        } str;
        struct {
            uint8_t   *bytes;
            uint32_t   len;
            int        owned;  /* 1 = kfree on release, 0 = pointer into AML */
        } buf;
        struct {
            aml_object_t *elems;
            uint32_t      n;
            int           owned;
        } pkg;
        struct {
            int sym_idx;       /* index into symbol table */
        } ref;
    } u;
};

/* Initialize the interpreter: walk every cataloged AML table and
 * register every Name/Method symbol with its full scoped path.
 * Idempotent. Returns the count of symbols registered. */
int aml_init(void);

/* Total symbols cataloged (Name + Method). */
int aml_symbol_count(void);

/* Evaluate the AML object at `path`. If it's a Method, args[] are
 * passed as Arg0..ArgN. If it's a Name, the value is returned and
 * args is ignored. result may be NULL if the caller doesn't care.
 *
 * Returns:
 *   0   success
 *  -1   path not found
 *  -2   evaluation hit an unimplemented opcode (logged to journal)
 *  -3   evaluation aborted (out-of-bounds, malformed AML, recursion limit)
 */
int aml_evaluate(const char *path, const aml_object_t *args, int nargs,
                 aml_object_t *result);

/* Release any heap memory owned by an object (buffers, packages with
 * owned=1). Safe to call on uninitialised objects. */
void aml_object_release(aml_object_t *o);

/* Selftest line: prints
 *   AML ................... N methods cataloged, M evaluable (S skipped)
 * The 'evaluable' count is determined by a dry-run pass that attempts
 * to evaluate every zero-arg Method and counts those that complete
 * without hitting an unimplemented opcode. */
void aml_print_selftest_line(void);

/* Journal access (per-invocation log: path, args, result, elapsed). */
typedef struct {
    char     path[64];
    int      nargs;
    uint64_t arg0;
    int      result_type;     /* aml_type_t */
    uint64_t result_int;
    int      rc;              /* aml_evaluate() return */
    uint64_t elapsed_tsc;
    uint64_t tsc_at_call;
} aml_journal_entry_t;

int  aml_journal_count(void);     /* number of entries currently in ring */
const aml_journal_entry_t *aml_journal_at(int idx); /* 0..count-1, newest at count-1 */

/* ── Symbol introspection (for cross-module discovery) ───────────── */

typedef enum {
    AML_SYM_NAME = 1,
    AML_SYM_METHOD,
    AML_SYM_OP_REGION,
    AML_SYM_FIELD_UNIT,
    AML_SYM_DEVICE,
} aml_sym_kind_t;

/* Total cataloged symbol slots (Name + Method + Device + Region + Field). */
int aml_sym_total(void);

/* Read kind + path of symbol at index. path_buf must be >= 64 bytes.
 * Returns 0 on success, -1 if idx is out of range. */
int aml_sym_info(int idx, aml_sym_kind_t *kind, char *path_buf, int path_buf_len);

/* OperationRegion(EmbeddedControl) hook. The interpreter calls this when
 * a Field declared on an EmbeddedControl region is read or written.
 * Implementation lives in ec.c. Returns 0 on success, -1 on failure
 * (no EC, timeout, etc). access_bytes is informational. */
int aml_ec_region_read(uint64_t base, uint32_t byte_off, int access_bytes,
                       uint64_t *out);
int aml_ec_region_write(uint64_t base, uint32_t byte_off, int access_bytes,
                        uint64_t val);

#endif /* ZEOS_AML_H */
