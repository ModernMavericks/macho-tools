/* atomic_write.c -- see atomic_write.h. Moved verbatim (mechanical rename to
 * the wa_ prefix only) from change_dylib.c, where copy_xattrs/write_in_place/
 * write_atomic first shipped. */

#include "atomic_write.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/xattr.h>

/* Copy every extended attribute from `src_path` onto the open descriptor
 * `dst_fd`. 10.9's <sys/xattr.h> has no fd-to-path or fd-to-fd copy call
 * (that's a Sierra-and-later copyfile(3) feature), so this is
 * listxattr+getxattr+fsetxattr by hand. Best-effort in the sense that it
 * keeps going past a single attribute's failure to try the rest, but it
 * DOES report failure to the caller -- silently dropping quarantine et al.
 * is exactly the bug being fixed here, so a failure is surfaced as a
 * warning rather than swallowed. A source with no xattr support at all
 * (ENOTSUP/ENOENT from the initial listxattr) is not an error. */
static int wa_copy_xattrs(const char *src_path, int dst_fd) {
    ssize_t listlen = listxattr(src_path, NULL, 0, 0);
    if (listlen < 0) return (errno == ENOTSUP || errno == ENOENT) ? 0 : -1;
    if (listlen == 0) return 0;

    char *names = (char *)malloc((size_t)listlen);
    if (!names) return -1;
    ssize_t got = listxattr(src_path, names, (size_t)listlen, 0);
    if (got < 0) { free(names); return -1; }

    int rc = 0;
    for (ssize_t off = 0; off < got; ) {
        const char *name = names + off;
        off += (ssize_t)strlen(name) + 1;

        ssize_t vlen = getxattr(src_path, name, NULL, 0, 0, 0);
        if (vlen < 0) { rc = -1; continue; }
        void *val = NULL;
        if (vlen > 0) {
            val = malloc((size_t)vlen);
            if (!val) { rc = -1; continue; }
            ssize_t got2 = getxattr(src_path, name, val, (size_t)vlen, 0, 0);
            if (got2 < 0) { free(val); rc = -1; continue; }
            vlen = got2;
        }
        if (fsetxattr(dst_fd, name, val, (size_t)vlen, 0, 0) != 0) rc = -1;
        free(val);
    }
    free(names);
    return rc;
}

int wa_is_input(const char *in, const char *out) {
    char rin[PATH_MAX], rout[PATH_MAX];
    if (realpath(in, rin) && realpath(out, rout) && strcmp(rin, rout) == 0) return 1;
    struct stat si, so;
    if (stat(in, &si) == 0 && stat(out, &so) == 0 &&
        si.st_dev == so.st_dev && si.st_ino == so.st_ino) return 1;
    return 0;
}

int wa_write_new(const char *in, const char *out, const uint8_t *buf, size_t size) {
    if (wa_is_input(in, out)) {
        fprintf(stderr, "%s: the output is the input; refusing to write it\n", out);
        return WA_IS_INPUT;
    }
    /* An existing `out` that is a symlink is followed, so the link keeps
     * pointing where it did and its target gets the new content. */
    char real[PATH_MAX];
    const char *target = (realpath(out, real) != NULL) ? real : out;

    struct stat ist;
    int have_in = (stat(in, &ist) == 0);

    size_t tlen = strlen(target) + 8;
    char *tmpl = (char *)malloc(tlen);
    if (!tmpl) { fprintf(stderr, "out of memory\n"); return WA_FAILED; }
    snprintf(tmpl, tlen, "%s.XXXXXX", target);
    int tfd = mkstemp(tmpl);
    if (tfd < 0) { perror("mkstemp"); free(tmpl); return WA_FAILED; }

    if (have_in) {
        fchmod(tfd, ist.st_mode & 07777);
        fchown(tfd, ist.st_uid, ist.st_gid);   /* best-effort: needs privilege */
    }
    if (wa_copy_xattrs(in, tfd) != 0)
        fprintf(stderr, "warning: %s: could not copy all extended attributes "
                        "(e.g. com.apple.quarantine) from %s\n", target, in);

    size_t off = 0;
    int failed = 0;
    while (off < size) {
        ssize_t n = write(tfd, buf + off, size - off);
        if (n < 0) { perror("write"); failed = 1; break; }
        off += (size_t)n;
    }
    if (!failed && fsync(tfd) != 0) { perror("fsync"); failed = 1; }
    close(tfd);
    if (failed || rename(tmpl, target) != 0) {
        if (!failed) perror("rename");
        unlink(tmpl);
        free(tmpl);
        return WA_FAILED;
    }
    free(tmpl);
    return 0;
}
