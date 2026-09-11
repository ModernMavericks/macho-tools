#ifndef MACHO9_EDIT_H
#define MACHO9_EDIT_H
/*
 * me_ -- applying an edit script (src/script.h) to one Mach-O: read the image
 * once, apply each statement in order to the in-memory buffer, verify the
 * result, and write it once.
 *
 * The property this exists for: if any statement is refused, or the image
 * fails verification, NOTHING is written -- the input is left exactly as it
 * was found, and an --output file is never created. That is what three tool
 * invocations, each writing the whole binary, could not promise.
 *
 * This module lowers and sequences; it performs no operation itself. Each
 * statement becomes a call to the one implementation of that operation --
 * src/rewrite.h's mr_apply_image for load-command/segment/dylib/rpath,
 * src/version_min.h, src/swift_retag.h and src/declassify.h's in-memory
 * cores for the other three -- the same code each CLI verb reaches.
 */
#include <stdio.h>

#include "script.h"

typedef struct {
    int   verbose;        /* log each statement to `log` as it runs */
    int   dry_run;        /* apply and verify, but do not write */
    FILE *log;            /* where the report goes; stderr in the CLI, and
                           * stderr when NULL */
} me_opts;

/* Applies `s` to `path`, verifies, and writes once -- to `out` if non-NULL,
 * else back to `path`. Returns 0 on success, MR_REFUSED (1) when a statement
 * or the verify declined on purpose, or MR_FAIL (2) for an operational
 * failure (a syscall, a malloc). On any non-zero return NOTHING has been
 * written -- with the one caveat about hard-linked destinations under WRITE,
 * below.
 *
 * NOT MR_ERROR: that is (-1), private to src/rewrite.c, and it is
 * mr_process_fat's per-slice status, not an exit code.
 *
 * INPUT. A thin 64-bit Mach-O only. A fat one is refused (MR_REFUSED), saying
 * so: `fixups set classic` converts one thin image, and applying a whole
 * script to each slice of a fat file is not supported. Anything else that is
 * not a readable 64-bit Mach-O is refused too; an input that cannot be
 * opened or read at all is MR_FAIL.
 *
 * VERIFY. mg_plausible (src/grow.h) runs over the finished image after the
 * last statement, every time, and a failure is a refusal. There is no
 * parameter and no environment variable that skips it -- MACHO_NO_VERIFY,
 * which mr_apply_image's own per-step check honours, is not consulted here.
 * So a script whose every statement succeeds can still be refused, including
 * one of nothing but segment renames, which that per-step check skips.
 *
 * WRITE. Through wa_write_atomic (src/atomic_write.h), with the input file's
 * mode: a temp file beside the destination, renamed over it. The destination
 * is written even when no statement changed anything, so `out` exists after
 * every successful run that is not a dry run. When the destination is a file
 * with more than one hard link, wa_write_atomic writes through the existing
 * inode instead, and a failure partway through THAT write can leave the file
 * truncated -- the one case in which a non-zero return does not guarantee
 * the destination is as it was; see atomic_write.h. Nothing is ever written
 * before verification has passed.
 *
 * REPORT, to o->log. Always printed: a statement's refusal,
 *   "macho9 edit: refused at statement K of N (line L); DEST left unmodified"
 * ("failed" in place of "refused" for MR_FAIL; K counts statements from 1, L
 * is the statement's line in the script), a refusal at the final verify, and
 * a dry run's
 *   "DEST: NOT written (--dry-run) -- would be N bytes".
 * Under o->verbose, each statement is also logged as
 * "  <kind> <op> <operands>" before it runs, and a run that gets that far
 * logs "PATH: verified" and "DEST: written (N bytes)". DEST is `out` when
 * given, else `path`. The operations keep printing their own progress to
 * stdout and their own refusals to stderr, exactly as they do for the CLI
 * verbs.
 *
 * fatal-warnings: an operation that matched nothing -- the same misses the
 * dylib/rpath/lc verbs report under their --fatal-warnings -- refuses the
 * run. Without it the miss is reported on stderr and the run continues.
 */
int me_run(const char *path, const char *out, const ms_script *s,
           const me_opts *o);

#endif /* MACHO9_EDIT_H */
