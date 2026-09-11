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

/* The statement vocabulary an edit script's operation lines are drawn from.
 * See src/script.c's MS_TABLE for the kind/op pairs actually accepted --
 * these enums just name the values ms_parse fills into an ms_stmt, and the
 * values a `switch` on .kind/.op matches against. */
enum { MS_LOAD_COMMAND, MS_SEGMENT, MS_VERSION_MIN, MS_SWIFT_ABI,
       MS_FIXUPS, MS_DYLIB, MS_RPATH };
enum { MS_DELETE, MS_RENAME, MS_SET, MS_REPLACE, MS_APPEND,
       MS_INSERT, MS_REEXPORT };

/* One operation line from an edit script. `a`/`.b` (NULL when the
 * statement's arity doesn't use them) point into the owning ms_script's
 * `text`, not into separately allocated storage. `line` is the 1-based
 * source line, for diagnostics raised later (e.g. by whatever applies the
 * script) that still need to name where a statement came from. */
typedef struct { int kind, op; const char *a, *b; int line; } ms_stmt;

/* A parsed edit script: every operation line (not directive lines -- those
 * only set the two flags below) in source order. */
typedef struct {
    ms_stmt *stmts;
    int      n;
    int      allow_grow;
    int      fatal_warnings;
    char    *text;      /* owns every operand's storage */
} ms_script;

/* Parses a whole edit script from `buf`/`len` (need not be NUL-terminated;
 * a final line with no trailing newline is fine, and there is no fixed cap
 * on the number of statements -- the array is sized from the script itself).
 * Every byte in [0, len) must be either tab, newline, or NOT an ASCII
 * control character -- so a NUL or a CR (or any other C0 control byte, or
 * DEL) is refused, not silently folded into an operand, while a byte 0x80
 * and above (part of UTF-8, say) is fine.
 *
 * On success, returns 0, fills `*out`, and the caller must eventually call
 * ms_free(out). On error, returns -1 and (if `err` and `errsz` are
 * non-zero) sets `err` to a message. On a PARSE error -- the script itself
 * is malformed -- that message names the offending 1-based source line as
 * "line N"; an allocation failure has no line to name, and says so instead.
 * Either way, ms_parse has already freed everything it allocated and zeroed
 * `*out` -- so ms_free(out) is not necessary after a failed ms_parse, though
 * it remains safe (a no-op) if called anyway. */
int ms_parse(const char *buf, size_t len, ms_script *out, char *err, size_t errsz);

/* Frees an ms_script filled by a successful ms_parse. Safe to call on an
 * ms_script that is all-zero (never parsed, or left by a failed ms_parse --
 * see ms_parse's own comment). */
void ms_free(ms_script *s);

/* Names an MS_* kind/op constant for diagnostics, e.g. ms_kind_name(MS_DYLIB)
 * -> "dylib". Returns "unknown" for a value outside the table -- defensive
 * only, since every kind/op a caller has came from this table in the first
 * place (either MS_TABLE's own constants, or an ms_stmt ms_parse filled). */
const char *ms_kind_name(int kind);
const char *ms_op_name(int op);

/* Enumerates the statement table row by row (0-based `i`), for a caller like
 * --capabilities that must generate its advertised vocabulary from the same
 * data ms_parse matches statements against, rather than maintaining a
 * second, hand-copied list that can drift out of agreement with this one.
 * Fills `*kind`, `*op`, `*nargs` and returns 1 for a valid row index;
 * returns 0 once `i` is past the last row, so a caller can loop
 * `for (i = 0; ms_table_row(i, &k, &o, &n); i++)`. */
int ms_table_row(int i, const char **kind, const char **op, int *nargs);

#endif
