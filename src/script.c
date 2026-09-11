#include "script.h"
#include <stdio.h>
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
