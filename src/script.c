#include "script.h"
#include "arch_names.h"
#include "lc_kinds.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int ms_err(char *err, size_t errsz, const char *msg) {
    if (err && errsz) snprintf(err, errsz, "%s", msg);
    return -1;
}

int ms_split(char *line, char **argv, int max, char *err, size_t errsz) {
    char *r = line;              /* read cursor */
    int   n = 0;
    for (;;) {
        while (*r == ' ' || *r == '\t') r++;
        if (*r == '\0' || *r == '#') break;   /* end of line, or a comment */
        if (n >= max) return ms_err(err, errsz, "too many fields on one line");

        char *w = r;             /* write cursor: always <= r, so in place */
        argv[n++] = w;
        while (*r && *r != ' ' && *r != '\t') {
            if (*r == '\'') {
                r++;
                while (*r != '\'') {
                    if (*r == '\0') return ms_err(err, errsz, "unterminated '");
                    *w++ = *r++;
                }
                r++;
            } else if (*r == '"') {
                r++;
                while (*r != '"') {
                    if (*r == '\0') return ms_err(err, errsz, "unterminated \"");
                    if (*r == '\\' && (r[1] == '"' || r[1] == '\\' ||
                                       r[1] == '$' || r[1] == '`')) r++;
                    *w++ = *r++;
                }
                r++;
            } else if (*r == '\\') {
                if (r[1] == '\0') return ms_err(err, errsz, "trailing backslash");
                r++;
                *w++ = *r++;
            } else {
                *w++ = *r++;
            }
        }
        /* r now points at the separator or the NUL. Capture it before the
         * terminator overwrites it -- w can equal r when nothing was
         * unquoted, and then *w = '\0' would clobber what we are about to
         * read. */
        char sep = *r;
        *w = '\0';
        if (sep == '\0') break;
        r++;
    }
    return n;
}

/* The statement table. Data, not a strcmp chain, because --capabilities is
 * GENERATED from these rows rather than maintained beside them -- this repo
 * has already had a defect from two such lists disagreeing (the DYLIB_OPS
 * table against the --capabilities text). Adding a statement here is the
 * whole of adding a statement. 14 rows: every "<kind> <op>" the spec
 * accepts. */
static const struct { const char *kind; int k; const char *op; int o; int nargs; }
MS_TABLE[] = {
    { "load-command", MS_LOAD_COMMAND, "delete",   MS_DELETE,   1 },
    { "segment",      MS_SEGMENT,      "rename",   MS_RENAME,   2 },
    { "version-min",  MS_VERSION_MIN,  "set",      MS_SET,      1 },
    { "swift-abi",    MS_SWIFT_ABI,    "set",      MS_SET,      1 },
    { "fixups",       MS_FIXUPS,       "set",      MS_SET,      1 },
    { "dylib",        MS_DYLIB,        "replace",  MS_REPLACE,  2 },
    { "dylib",        MS_DYLIB,        "append",   MS_APPEND,   1 },
    { "dylib",        MS_DYLIB,        "insert",   MS_INSERT,   1 },
    { "dylib",        MS_DYLIB,        "delete",   MS_DELETE,   1 },
    { "dylib",        MS_DYLIB,        "reexport", MS_REEXPORT, 1 },
    { "rpath",        MS_RPATH,        "replace",  MS_REPLACE,  2 },
    { "rpath",        MS_RPATH,        "delete",   MS_DELETE,   1 },
    { "rpath",        MS_RPATH,        "append",   MS_APPEND,   1 },
    { "rpath",        MS_RPATH,        "insert",   MS_INSERT,   1 },
};
static const int MS_TABLE_N = (int)(sizeof MS_TABLE / sizeof MS_TABLE[0]);

int ms_table_row(int i, const char **kind, const char **op, int *nargs) {
    if (i < 0 || i >= MS_TABLE_N) return 0;
    *kind = MS_TABLE[i].kind; *op = MS_TABLE[i].op; *nargs = MS_TABLE[i].nargs;
    return 1;
}

const char *ms_kind_name(int kind) {
    int i;
    for (i = 0; i < MS_TABLE_N; i++)
        if (MS_TABLE[i].k == kind) return MS_TABLE[i].kind;
    return "unknown";
}

const char *ms_op_name(int op) {
    int i;
    for (i = 0; i < MS_TABLE_N; i++)
        if (MS_TABLE[i].o == op) return MS_TABLE[i].op;
    return "unknown";
}

/* Room for a statement's kind, op, and both operands, plus slack above the
 * largest accepted arity (2) so that a handful of stray extra fields on an
 * otherwise-recognizable line is matched against MS_TABLE and refused for
 * its arity (see test_extra_fields_report_arity_not_overflow in
 * tests/script_test.c), rather than every overlong line getting the same
 * generic "too many fields on one line" from ms_split. No accepted
 * statement needs more than 4 fields; a line with more than MS_MAX_FIELDS
 * still falls back to that generic message. */
#define MS_MAX_FIELDS 16

/* Formats "line N: " + the given message into err (best-effort; silently
 * truncated if errsz is too small, same as every other ms_err use in this
 * file), THEN frees `stmts` and `text` and zeros *out, and returns -1.
 *
 * The order matters: `fmt`'s varargs are frequently a field straight out of
 * the line just rejected (e.g. fields[0]), and those fields point into
 * `text` -- so formatting has to happen before `text` is freed, not after.
 * Freeing here rather than at each call site also means every ms_parse
 * failure that stems from a specific source line goes through the exact
 * same sequence, so none of them can get the order wrong, forget to free,
 * or forget the "line N" prefix the spec requires. `stmts` may be NULL
 * (cap == 0: pass 1 found no non-blank, non-comment line); `text` is never
 * NULL here (pass 2 only starts once it's allocated). free(NULL) is a
 * no-op either way. */
static int ms_failf(ms_stmt *stmts, char *text, ms_script *out,
                     char *err, size_t errsz, int line, const char *fmt, ...) {
    if (err && errsz) {
        int off = snprintf(err, errsz, "line %d: ", line);
        if (off > 0 && (size_t)off < errsz) {
            va_list ap;
            va_start(ap, fmt);
            vsnprintf(err + off, errsz - (size_t)off, fmt, ap);
            va_end(ap);
        }
    }
    free(stmts);
    free(text);
    memset(out, 0, sizeof *out);
    return -1;
}

int ms_parse(const char *buf, size_t len, ms_script *out, char *err, size_t errsz) {
    memset(out, 0, sizeof *out);

    /* Pass 1: an upper bound on the statement count, from a disposable
     * per-line copy. It does not try to tell a directive line from a
     * statement line -- that distinction doesn't matter yet, since counting
     * a directive only costs the final array one unused slot, which is
     * harmless, and skipping the distinction keeps this pass simple. It
     * likewise does not fail when ms_split reports a syntax error on some
     * line: it just doesn't count that line, and moves on. Reporting is
     * pass 2's job entirely (see pass 2's own comment for why), so a script
     * with an early semantic error and a later syntax error still gets the
     * earlier one reported, in source order, regardless of which pass would
     * otherwise have noticed which problem first. There is no fixed cap:
     * the array is sized from the script, however long that is. */
    char *scratch = len ? malloc(len + 1) : NULL;
    if (len && !scratch) return ms_err(err, errsz, "out of memory");
    int cap = 0;
    {
        size_t i = 0;
        while (i < len) {
            size_t start = i;
            while (i < len && buf[i] != '\n') i++;
            size_t linelen = i - start;
            char *fields[MS_MAX_FIELDS];
            int n;
            memcpy(scratch, buf + start, linelen);
            scratch[linelen] = '\0';
            if (i < len) i++;   /* skip the newline */
            n = ms_split(scratch, fields, MS_MAX_FIELDS, NULL, 0);
            if (n > 0) cap++;
        }
    }
    free(scratch);

    ms_stmt *stmts = cap ? malloc(sizeof(ms_stmt) * (size_t)cap) : NULL;
    if (cap && !stmts) return ms_err(err, errsz, "out of memory");

    char *text = malloc(len + 1);
    if (!text) { free(stmts); return ms_err(err, errsz, "out of memory"); }
    memcpy(text, buf, len);
    text[len] = '\0';

    /* Pass 2: fills the array, this time from `text` itself -- every
     * ms_stmt.a/.b ends up pointing straight into it, so ms_free frees
     * exactly two allocations (this and `stmts`). This is the ONLY pass
     * that reports an error, and it walks the script strictly in source
     * order, one line at a time, so the FIRST line with a problem is always
     * the one reported -- never a later line whose problem some earlier
     * check happened to notice first. Within a single line, the checks
     * still run in a fixed order -- an embedded control byte, then
     * ms_split's syntax, then the semantic rules (unknown statement, wrong
     * arity, a directive out of place, a value outside the accepted
     * vocabulary) -- so if a line manages to fail more than one of those,
     * which one gets reported depends on that order, not on source
     * position (they're all on the same line). */
    int n_stmts = 0;
    int allow_grow = 0, fatal_warnings = 0, seen_operation = 0;
    unsigned arch_mask = 0;
    size_t i = 0;
    int lineno = 0;
    while (i < len) {
        lineno++;
        size_t start = i;
        /* A NUL or other control byte (CR included) partway through a line
         * would make ms_split stop early and silently hand back a truncated
         * field instead of the error this is -- so this has to catch it
         * BEFORE ms_split ever sees the line. Tab is a field separator and
         * newline is the line separator; both are fine. */
        while (i < len && text[i] != '\n') {
            unsigned char c = (unsigned char)text[i];
            if (c == '\0' || (c < 0x20 && c != '\t') || c == 0x7f)
                return ms_failf(stmts, text, out, err, errsz, lineno,
                    "control character");
            i++;
        }
        int had_nl = (i < len);
        char *line = text + start;
        char *fields[MS_MAX_FIELDS];
        char lerr[128] = {0};
        int n, t, found, nargs;

        text[i] = '\0';
        if (had_nl) i++;

        n = ms_split(line, fields, MS_MAX_FIELDS, lerr, sizeof lerr);
        if (n < 0)
            return ms_failf(stmts, text, out, err, errsz, lineno, "%s", lerr);
        if (n == 0) continue;   /* blank or comment */

        if (strcmp(fields[0], "arch") == 0) {
            if (n != 2)
                return ms_failf(stmts, text, out, err, errsz, lineno,
                    "directive 'arch' takes exactly one operand, an arch name");
            if (seen_operation)
                return ms_failf(stmts, text, out, err, errsz, lineno,
                    "directive 'arch' must precede every operation");
            int row = ma_lookup(fields[1]);
            if (row < 0) {
                char names[128];
                ma_list(names, sizeof names);
                return ms_failf(stmts, text, out, err, errsz, lineno,
                    "unknown arch '%s' (expected one of: %s)", fields[1], names);
            }
            arch_mask |= 1u << row;
            continue;
        }

        if (strcmp(fields[0], "allow-grow") == 0 ||
            strcmp(fields[0], "fatal-warnings") == 0) {
            if (n != 1)
                return ms_failf(stmts, text, out, err, errsz, lineno,
                    "directive '%s' takes no operands", fields[0]);
            if (seen_operation)
                return ms_failf(stmts, text, out, err, errsz, lineno,
                    "directive '%s' must precede every operation", fields[0]);
            if (strcmp(fields[0], "allow-grow") == 0) allow_grow = 1;
            else fatal_warnings = 1;
            continue;
        }

        if (n < 2)
            return ms_failf(stmts, text, out, err, errsz, lineno,
                "unknown statement '%s'", fields[0]);

        found = -1;
        for (t = 0; t < MS_TABLE_N; t++) {
            if (strcmp(fields[0], MS_TABLE[t].kind) == 0 &&
                strcmp(fields[1], MS_TABLE[t].op) == 0) { found = t; break; }
        }
        if (found < 0)
            return ms_failf(stmts, text, out, err, errsz, lineno,
                "unknown statement '%s %s'", fields[0], fields[1]);

        nargs = n - 2;
        if (nargs != MS_TABLE[found].nargs)
            return ms_failf(stmts, text, out, err, errsz, lineno,
                "%s %s takes %d argument%s (got %d)", fields[0], fields[1],
                MS_TABLE[found].nargs, MS_TABLE[found].nargs == 1 ? "" : "s",
                nargs);

        {
            int kind = MS_TABLE[found].k, op = MS_TABLE[found].o;

            if (kind == MS_LOAD_COMMAND && op == MS_DELETE) {
                uint32_t cmd;
                if (lc_kind_by_name(fields[2], &cmd) != 0)
                    return ms_failf(stmts, text, out, err, errsz, lineno,
                        "load-command delete: unknown kind '%s'", fields[2]);
            } else if (kind == MS_VERSION_MIN && op == MS_SET &&
                       strcmp(fields[2], "10.9") != 0) {
                return ms_failf(stmts, text, out, err, errsz, lineno,
                    "version-min set accepts only '10.9' (got '%s')", fields[2]);
            } else if (kind == MS_SWIFT_ABI && op == MS_SET &&
                       strcmp(fields[2], "legacy") != 0) {
                return ms_failf(stmts, text, out, err, errsz, lineno,
                    "swift-abi set accepts only 'legacy' (got '%s')", fields[2]);
            } else if (kind == MS_FIXUPS && op == MS_SET &&
                       strcmp(fields[2], "classic") != 0) {
                return ms_failf(stmts, text, out, err, errsz, lineno,
                    "fixups set accepts only 'classic' (got '%s')", fields[2]);
            }

            stmts[n_stmts].kind = kind;
            stmts[n_stmts].op = op;
            stmts[n_stmts].a = nargs >= 1 ? fields[2] : NULL;
            stmts[n_stmts].b = nargs >= 2 ? fields[3] : NULL;
            stmts[n_stmts].line = lineno;
            n_stmts++;
            seen_operation = 1;
        }
    }

    out->stmts = stmts;
    out->n = n_stmts;
    out->allow_grow = allow_grow;
    out->fatal_warnings = fatal_warnings;
    out->arch_mask = arch_mask;
    out->text = text;
    return 0;
}

void ms_free(ms_script *s) {
    if (!s) return;
    free(s->stmts);
    free(s->text);
    s->stmts = NULL;
    s->text = NULL;
    s->n = 0;
}
