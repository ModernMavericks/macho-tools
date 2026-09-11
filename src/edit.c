/*
 * me_ -- see edit.h. Read once, apply each statement in order, verify,
 * write once.
 *
 * Every operation is performed by the code that performs it for the CLI
 * verbs; what lives here is the lowering from a statement to that call (the
 * switch in me_apply), the sequencing, the final verify and the single
 * write. The operations print what they have always printed, to stdout and
 * stderr; this module's own report goes to me_opts.log.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <mach-o/fat.h>

#include "edit.h"
#include "image.h"
#include "grow.h"
#include "rewrite.h"
#include "segname.h"
#include "lc_kinds.h"
#include "version_min.h"
#include "swift_retag.h"
#include "declassify.h"
#include "atomic_write.h"
#include "mach_compat.h"

/* "208526708" -> "208,526,708", the way the report prints a byte count. */
static void me_commas(char out[32], size_t n) {
    char digits[24];
    int len = snprintf(digits, sizeof digits, "%zu", n);
    int o = 0;
    for (int i = 0; i < len; i++) {
        if (i > 0 && (len - i) % 3 == 0) out[o++] = ',';
        out[o++] = digits[i];
    }
    out[o] = '\0';
}

static void me_log_stmt(FILE *log, const ms_stmt *st) {
    fprintf(log, "  %s %s", ms_kind_name(st->kind), ms_op_name(st->op));
    if (st->a) fprintf(log, " %s", st->a);
    if (st->b) fprintf(log, " %s", st->b);
    fprintf(log, "\n");
}

/* One mr_ops through the rewrite, then the verdict on anything it asked for
 * that matched nothing: a report on stderr, and a refusal under
 * fatal-warnings (ops->fatal_unmatched). The hit counts are per statement,
 * because each statement is its own rewrite of the image as it now stands. */
static int me_rewrite(uint8_t **pbuf, size_t *psize, const char *path, const mr_ops *ops) {
    mr_hits hits;
    int modified = 0;
    memset(&hits, 0, sizeof hits);
    int rc = mr_apply_image(pbuf, psize, path, ops, &modified, &hits);
    if (rc != 0) return rc;
    return mr_unmatched_verdict(ops, &hits);
}

/* The version-min and swift-abi cores take an mi_image; the buffer is the
 * image as the previous statement left it, so it gets the same validation
 * mi_open would give a file. */
static int me_view(uint8_t *buf, size_t size, mi_image *im, const char *path, FILE *log) {
    if (mi_wrap(buf, size, im) == 0) return 0;
    fprintf(log, "macho9 edit: %s: the image is no longer a readable 64-bit Mach-O\n", path);
    return MR_REFUSED;
}

/* The lowering: one statement, one call to the code that performs it. Returns
 * 0, MR_REFUSED or MR_FAIL. *pbuf and *psize always name the current image
 * afterwards, whether or not the statement succeeded, because two of these
 * reallocate it: allow-grow's header grow inside the rewrite, and the room
 * `fixups set classic` appends its opcode streams into. */
static int me_apply(uint8_t **pbuf, size_t *psize, const char *path,
                    const ms_script *s, const ms_stmt *st, FILE *log) {
    mr_ops ops;
    mr_change change;
    memset(&ops, 0, sizeof ops);
    memset(&change, 0, sizeof change);
    ops.fatal_unmatched = s->fatal_warnings;

    switch (st->kind) {
    case MS_LOAD_COMMAND: {
        /* ms_parse has already refused a KIND outside LC_STRIP_KINDS; this
         * is the same lookup, as cmd_lc makes it. */
        uint32_t cmd = 0;
        if (lc_kind_by_name(st->a, &cmd) != 0) break;
        ops.strip_cmds = &cmd;
        ops.n_strip_cmds = 1;
        return me_rewrite(pbuf, psize, path, &ops);
    }

    case MS_SEGMENT: {
        /* The same pre-check cmd_segment makes: a segname field is 16 bytes,
         * and mseg_rename_lc would truncate a longer name silently. */
        if (!mseg_name_fits(st->b)) {
            fprintf(log, "macho9 edit: new segment name '%s' is longer than the %d bytes "
                         "a segname field holds\n", st->b, MSEG_NAME_MAX);
            return MR_REFUSED;
        }
        /* A rename has no hit array for mr_unmatched_verdict to read; its
         * match count comes back through segment_renamed, as it does for
         * cmd_segment. Zero is this statement's miss: reported on stderr in
         * the shape of the other "matched nothing" lines, and a refusal
         * under fatal-warnings -- before anything is written, because
         * nothing is written until after the last statement. */
        int renamed = 0;
        ops.segment_rename_old = st->a;
        ops.segment_rename_new = st->b;
        ops.segment_renamed = &renamed;
        int rc = me_rewrite(pbuf, psize, path, &ops);
        if (rc != 0 || renamed > 0) return rc;
        fprintf(stderr, "macho9: segment %s matched nothing\n", st->a);
        return s->fatal_warnings ? MR_REFUSED : 0;
    }

    case MS_DYLIB:
    case MS_RPATH: {
        /* mr_change's own encoding (rewrite.h), the one cmd_dylib_or_rpath
         * also produces: new_path NULL deletes, "" with reexport promotes. */
        int rpath = (st->kind == MS_RPATH);
        const char *const *one = &st->a;
        switch (st->op) {
        case MS_REPLACE:  change.old_path = st->a; change.new_path = st->b; break;
        case MS_DELETE:   change.old_path = st->a; change.new_path = NULL;  break;
        case MS_REEXPORT: if (rpath) goto unknown;
                          change.old_path = st->a; change.new_path = "";
                          change.reexport = 1; break;
        case MS_APPEND:
            if (rpath) { ops.rpath_appends = one; ops.n_rpath_appends = 1; }
            else       { ops.dylib_appends = one; ops.n_dylib_appends = 1; }
            break;
        case MS_INSERT:
            if (rpath) { ops.rpath_inserts = one; ops.n_rpath_inserts = 1; }
            else       { ops.dylib_inserts = one; ops.n_dylib_inserts = 1; }
            break;
        default: goto unknown;
        }
        if (change.old_path) {
            if (rpath) { ops.rpath_changes = &change; ops.n_rpath_changes = 1; }
            else       { ops.dylib_changes = &change; ops.n_dylib_changes = 1; }
        }
        /* allow-grow reaches only the statements that can outgrow the header
         * pad. Setting it on a segment rename or a load-command delete would
         * grow nothing and would, for the rename, switch off
         * mr_is_rename_only's scoping of the rewrite's own plausibility
         * check. */
        ops.allow_grow = s->allow_grow;
        return me_rewrite(pbuf, psize, path, &ops);
    }

    case MS_VERSION_MIN: {
        /* ms_parse accepts only 10.9, and 10.9 is the only floor this core
         * writes; the parser's value check is the one place that says so. */
        mi_image im;
        int added = 0;
        if (me_view(*pbuf, *psize, &im, path, log) != 0) return MR_REFUSED;
        return mv_add_version_min_image(&im, &added);
    }

    case MS_SWIFT_ABI: {
        /* `legacy` is the only value ms_parse accepts. A count of zero is
         * not a refusal: an image with no Swift classes has nothing to
         * retag, as `macho9 retag-swift` reports with exit 0. */
        mi_image im;
        if (me_view(*pbuf, *psize, &im, path, log) != 0) return MR_REFUSED;
        (void)mswift_retag_image(&im);
        return 0;
    }

    case MS_FIXUPS: {
        /* The conversion appends into room past the image rather than
         * reallocating (declassify.h), and md_declassify gets that room by
         * reading the file with MDCL_SLACK to spare. The image is already in
         * memory here, so the room comes from a realloc instead -- zeroed,
         * because the streams are aligned within it and the alignment gap
         * becomes part of the output. */
        size_t len = *psize, newlen = 0;
        if (len > SIZE_MAX - MDCL_SLACK) {
            fprintf(log, "macho9 edit: %s: too large to make room for fixups set classic\n", path);
            return MR_FAIL;
        }
        uint8_t *nb = (uint8_t *)realloc(*pbuf, len + MDCL_SLACK);
        if (!nb) {
            fprintf(log, "macho9 edit: out of memory making room for fixups set classic\n");
            return MR_FAIL;
        }
        *pbuf = nb;
        memset(nb + len, 0, MDCL_SLACK);
        int rc = md_declassify_buf(nb, len, len + MDCL_SLACK, &newlen);
        /* Tested by name, as declassify.h requires: PASSTHROUGH is a nonzero
         * success. */
        if (rc == MDCL_CONVERTED || rc == MDCL_PASSTHROUGH) { *psize = newlen; return 0; }
        if (rc == MDCL_REFUSED) return MR_REFUSED;   /* the reason is on stderr */
        if (rc == MDCL_ERROR) return MR_FAIL;        /* likewise */
        if (rc == MDCL_NOT_MACHO) {
            fprintf(log, "macho9 edit: %s: the image is no longer a readable 64-bit Mach-O\n", path);
            return MR_REFUSED;
        }
        fprintf(log, "macho9 edit: md_declassify_buf returned an unrecognized code %d\n", rc);
        return MR_FAIL;
    }
    }

unknown:
    /* Unreachable through ms_parse, which refuses a statement outside
     * MS_TABLE; a statement this switch does not lower is a build that
     * disagrees with itself, not something the image did. */
    fprintf(log, "macho9 edit: cannot apply '%s %s'\n", ms_kind_name(st->kind), ms_op_name(st->op));
    return MR_FAIL;
}

/* mi_open said MI_NOT_MACHO. Say which kind of "no": a fat file is a Mach-O,
 * just not one this verb edits, and deserves to be told so rather than
 * called "not a Mach-O". */
static int me_refuse_input(const char *path, FILE *log) {
    uint32_t magic = 0;
    int fd = open(path, O_RDONLY);
    ssize_t got = fd >= 0 ? read(fd, &magic, sizeof magic) : -1;
    if (fd >= 0) close(fd);
    if (got == (ssize_t)sizeof magic &&
        (magic == FAT_MAGIC || magic == FAT_CIGAM ||
         magic == FAT_MAGIC_64 || magic == FAT_CIGAM_64)) {
        fprintf(log, "macho9 edit: %s is a fat (universal) Mach-O; edit applies a script to "
                     "one thin 64-bit image. `fixups set classic` converts a thin image only, "
                     "and running a script over each slice is not supported -- extract one "
                     "with `lipo -thin ARCH` first\n", path);
    } else {
        fprintf(log, "macho9 edit: %s: not a readable 64-bit Mach-O\n", path);
    }
    return MR_REFUSED;
}

int me_run(const char *path, const char *out, const ms_script *s, const me_opts *o) {
    FILE *log = (o && o->log) ? o->log : stderr;
    int verbose = o ? o->verbose : 0;
    int dry_run = o ? o->dry_run : 0;
    const char *dest = out ? out : path;
    char bytes[32];

    /* Read the image once. */
    mi_image im;
    int mo = mi_open(path, &im);
    if (mo == MI_IO_ERROR) {
        fprintf(log, "macho9 edit: %s: cannot open or read\n", path);
        return MR_FAIL;
    }
    if (mo != 0) return me_refuse_input(path, log);
    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(log, "macho9 edit: %s: cannot stat\n", path);
        mi_close(&im);
        return MR_FAIL;
    }
    size_t size = im.size;
    uint8_t *buf = mi_release(&im);

    /* Apply each statement in order, each to the image the one before it
     * left. WHY NOT BATCH THEM into one operation set, the way `macho9
     * dylib` batches its flags into one mr_ops: because `fixups set classic`
     * cannot batch with anything -- every later statement has to see the
     * lowered image, with its new LC_DYLD_INFO_ONLY and extended __LINKEDIT
     * -- and because running in sequence is what a reader of the script
     * assumes. `dylib append X` followed by `dylib replace X Y` means
     * something only in sequence. The cost is rebuilding the load-command
     * table once per statement: a few KB, against I/O that happens once
     * either way. */
    for (int i = 0; i < s->n; i++) {
        const ms_stmt *stmt = &s->stmts[i];
        if (verbose) me_log_stmt(log, stmt);
        int rc = me_apply(&buf, &size, path, s, stmt, log);
        if (rc != 0) {
            if (rc != MR_REFUSED) rc = MR_FAIL;
            fprintf(log, "macho9 edit: %s at statement %d of %d (line %d); %s left unmodified\n",
                    rc == MR_REFUSED ? "refused" : "failed", i + 1, s->n, stmt->line, dest);
            free(buf);
            return rc;
        }
    }

    /* Verify the finished image: always, and never subject to
     * MACHO_NO_VERIFY. A failure is a refusal -- including an allocation
     * failure inside mg_plausible, which it reports the same way as every
     * other reason it declines (see rewrite.c's comment on that fold). */
    if (mg_plausible(buf, size) != 0) {
        fprintf(log, "macho9 edit: refused at verification, after statement %d of %d; "
                     "%s left unmodified\n", s->n, s->n, dest);
        free(buf);
        return MR_REFUSED;
    }
    if (verbose) fprintf(log, "%s: verified\n", path);

    me_commas(bytes, size);
    if (dry_run) {
        fprintf(log, "%s: NOT written (--dry-run) -- would be %s bytes\n", dest, bytes);
        free(buf);
        return 0;
    }

    /* Write once. */
    if (wa_write_atomic(dest, st.st_mode, buf, size) != 0) {
        fprintf(log, "macho9 edit: %s left unmodified (write failed)\n", dest);
        free(buf);
        return MR_FAIL;
    }
    if (verbose) fprintf(log, "%s: written (%s bytes)\n", dest, bytes);
    free(buf);
    return 0;
}
