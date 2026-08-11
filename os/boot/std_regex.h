/*
 * Zeos — std.regex
 *
 * Minimal regex engine.  Supported metas:
 *   ^ $ . * + ?     anchors and quantifiers
 *   \d \w \s        digit / word / whitespace classes
 *   [abc] [a-z]     character classes (no negation)
 *   ( ... | ... )   one level of alternation per group
 *
 * Out of scope: backreferences (\1), look-arounds, UTF-8.
 *
 * Implementation: backtracking matcher.  Worst-case exponential on
 * adversarial patterns; fine for simple kernel use.
 */

#ifndef ZEOS_STD_REGEX_H
#define ZEOS_STD_REGEX_H

#include <stdint.h>

/* Returns 1 if the pattern matches anywhere in s, 0 otherwise. */
int zp_regex_match_str(const char *s, const char *pattern);

/* Returns first match offset in s, or -1 on miss. */
int zp_regex_find_str(const char *s, const char *pattern);

/* Replace first match in s with `repl`.  Writes into out (out_max bytes,
 * NUL terminated).  Returns bytes written on success, -1 on miss / out
 * of room. */
int zp_regex_replace_str(const char *s, const char *pattern,
                         const char *repl, char *out, int out_max);

#endif
