/*
 * Zeos — std.json
 *
 * Parse JSON text into a tagged Z+ value (int / str / struct / array)
 * and emit a value back into a JSON-shaped string.  Real recursive-
 * descent parser; no streaming, no schemas, no floats (yet).
 *
 * Returned handles live in the standard zplus pools (zp_str_intern /
 * zp_struct_alloc / zp_array_alloc) so the caller's chain wires the
 * result like any other Z+ value.
 */

#ifndef ZEOS_STD_JSON_H
#define ZEOS_STD_JSON_H

#include <stdint.h>

/* Parse `s`. Returns a Z+ wire int32 packed as
 *   high byte  = zp_val_kind (0=int, 1=str, 2=struct, 3=array)
 *   low 24-bit = pool handle (or signed int for INT)
 * On parse failure returns 0 (kind=INT, value=0) and writes a kprint diag.
 */
int32_t zp_json_parse_str(const char *s);

/* Emit a Z+ value (kind + handle) as a JSON string.  Returns a string
 * pool handle (or -1 on failure). */
int     zp_json_emit_value(int kind, int handle);

#endif /* ZEOS_STD_JSON_H */
