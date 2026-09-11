#include "script.h"
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

/* Room for a statement's kind, op, and both operands, plus enough slack that
 * a malformed line reports as the right error (unknown statement / wrong
 * arity) rather than "too many fields on one line" swallowing a case the
 * tests care about naming precisely. No accepted statement needs more than
 * 4 fields. */
#define MS_MAX_FIELDS 16

/* Zeros *out, formats "line N: " + the given message into err (best-effort;
 * silently truncated if errsz is too small, same as every other ms_err use
 * in this file), and returns -1. Every ms_parse failure that stems from a
 * specific source line goes through here so none of them forget either the
 * zeroing or the "line N" prefix the spec requires. */
static int ms_failf(ms_script *out, char *err, size_t errsz, int line, const char *fmt, ...) {
    memset(out, 0, sizeof *out);
    if (err && errsz) {
        int off = snprintf(err, errsz, "line %d: ", line);
        if (off > 0 && (size_t)off < errsz) {
            va_list ap;
            va_start(ap, fmt);
            vsnprintf(err + off, errsz - (size_t)off, fmt, ap);
            va_end(ap);
        }
    }
    return -1;
}

int ms_parse(const char *buf, size_t len, ms_script *out, char *err, size_t errsz) {
    memset(out, 0, sizeof *out);

    /* Pass 1: an upper bound on the statement count, from a disposable
     * per-line copy. Directive lines (allow-grow, fatal-warnings) count too,
     * since telling them apart from statements needs the same field split
     * pass 2 does anyway -- so this may over-allocate by a few slots, never
     * under, and there is no fixed cap: the array is sized from the script,
     * however long that is. */
    char *scratch = len ? malloc(len + 1) : NULL;
    if (len && !scratch) return ms_err(err, errsz, "out of memory");
    int cap = 0;
    {
        size_t i = 0;
        int lineno = 0;
        while (i < len) {
            lineno++;
            size_t start = i;
            while (i < len && buf[i] != '\n') i++;
            size_t linelen = i - start;
            char *fields[MS_MAX_FIELDS];
            char lerr[128] = {0};
            int n;
            memcpy(scratch, buf + start, linelen);
            scratch[linelen] = '\0';
            if (i < len) i++;   /* skip the newline */
            n = ms_split(scratch, fields, MS_MAX_FIELDS, lerr, sizeof lerr);
            if (n < 0) {
                free(scratch);
                return ms_failf(out, err, errsz, lineno, "%s", lerr);
            }
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
     * exactly two allocations (this and `stmts`). */
    int n_stmts = 0;
    int allow_grow = 0, fatal_warnings = 0, seen_operation = 0;
    size_t i = 0;
    int lineno = 0;
    while (i < len) {
        lineno++;
        size_t start = i;
        while (i < len && text[i] != '\n') i++;
        int had_nl = (i < len);
        char *line = text + start;
        char *fields[MS_MAX_FIELDS];
        char lerr[128] = {0};
        int n, t, found, nargs;

        text[i] = '\0';
        if (had_nl) i++;

        n = ms_split(line, fields, MS_MAX_FIELDS, lerr, sizeof lerr);
        if (n < 0) {
            free(stmts); free(text);
            return ms_failf(out, err, errsz, lineno, "%s", lerr);
        }
        if (n == 0) continue;   /* blank or comment */

        if (strcmp(fields[0], "allow-grow") == 0 ||
            strcmp(fields[0], "fatal-warnings") == 0) {
            if (n != 1) {
                free(stmts); free(text);
                return ms_failf(out, err, errsz, lineno,
                    "directive '%s' takes no operands", fields[0]);
            }
            if (seen_operation) {
                free(stmts); free(text);
                return ms_failf(out, err, errsz, lineno,
                    "directive '%s' must precede every operation", fields[0]);
            }
            if (strcmp(fields[0], "allow-grow") == 0) allow_grow = 1;
            else fatal_warnings = 1;
            continue;
        }

        if (n < 2) {
            free(stmts); free(text);
            return ms_failf(out, err, errsz, lineno,
                "unknown statement '%s'", fields[0]);
        }

        found = -1;
        for (t = 0; t < MS_TABLE_N; t++) {
            if (strcmp(fields[0], MS_TABLE[t].kind) == 0 &&
                strcmp(fields[1], MS_TABLE[t].op) == 0) { found = t; break; }
        }
        if (found < 0) {
            free(stmts); free(text);
            return ms_failf(out, err, errsz, lineno,
                "unknown statement '%s %s'", fields[0], fields[1]);
        }

        nargs = n - 2;
        if (nargs != MS_TABLE[found].nargs) {
            free(stmts); free(text);
            return ms_failf(out, err, errsz, lineno,
                "%s %s takes %d argument%s (got %d)", fields[0], fields[1],
                MS_TABLE[found].nargs, MS_TABLE[found].nargs == 1 ? "" : "s",
                nargs);
        }

        {
            int kind = MS_TABLE[found].k, op = MS_TABLE[found].o;

            if (kind == MS_LOAD_COMMAND && op == MS_DELETE) {
                size_t k;
                int ok = 0;
                for (k = 0; k < LC_STRIP_KINDS_COUNT; k++)
                    if (strcmp(fields[2], LC_STRIP_KINDS[k].name) == 0) { ok = 1; break; }
                if (!ok) {
                    free(stmts); free(text);
                    return ms_failf(out, err, errsz, lineno,
                        "load-command delete: unknown kind '%s'", fields[2]);
                }
            } else if (kind == MS_VERSION_MIN && op == MS_SET &&
                       strcmp(fields[2], "10.9") != 0) {
                free(stmts); free(text);
                return ms_failf(out, err, errsz, lineno,
                    "version-min set accepts only '10.9' (got '%s')", fields[2]);
            } else if (kind == MS_SWIFT_ABI && op == MS_SET &&
                       strcmp(fields[2], "legacy") != 0) {
                free(stmts); free(text);
                return ms_failf(out, err, errsz, lineno,
                    "swift-abi set accepts only 'legacy' (got '%s')", fields[2]);
            } else if (kind == MS_FIXUPS && op == MS_SET &&
                       strcmp(fields[2], "classic") != 0) {
                free(stmts); free(text);
                return ms_failf(out, err, errsz, lineno,
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
