#ifndef MACHO9_SCRIPT_H
#define MACHO9_SCRIPT_H

#include <stddef.h>

/* Splits ONE line into fields using shell word rules, in place.
 *
 * The rules are chosen to agree exactly with compat/translate.sh's mt_quote,
 * which is the generator for these scripts: it emits a bare word only when
 * every character is in [A-Za-z0-9_@%+=:,./-], and otherwise single-quotes
 * the whole word with each ' written as '\''. So an unquoted '#' can mean
 * "comment" without ever eating a real path -- mt_quote never emits one.
 *
 *   - fields separate on unquoted space or tab
 *   - '...'  literal; no escapes inside (mt_quote's '\'' works because the
 *            quote closes, \' is a literal quote outside quotes, and the
 *            next ' reopens)
 *   - "..."  backslash escapes \" \\ \$ \` ; any other backslash is literal
 *   - \x     outside quotes: literal x
 *   - #      unquoted, at the start of a field: comment to end of line
 *
 * `line` is modified: each returned field is NUL-terminated in place.
 * Returns the field count, 0 for a blank or comment-only line, or -1 with
 * `err` set on a quoting error or on more than `max` fields. Overflow is an
 * error rather than truncation: silently dropping an operand is the
 * silent-success class this toolkit exists to eliminate. */
int ms_split(char *line, char **argv, int max, char *err, size_t errsz);

#endif
