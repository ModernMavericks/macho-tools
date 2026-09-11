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
    int   verbose;        /* log each statement to `log` as it runs, and
                           * beneath it any follow-up work it did (see
                           * REPORT, below) */
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
 * is the statement's line in the script), a refusal at the final verify, a
 * failed write,
 *   "macho9 edit: DEST left unmodified (write failed)",
 * and a dry run's
 *   "DEST: NOT written (--dry-run) -- would be N bytes".
 * Under o->verbose, each statement is also logged as
 * "  <kind> <op> <operands>" before it runs, and a run that gets that far
 * logs "PATH: verified" and "DEST: written (N bytes)". DEST is `out` when
 * given, else `path`. The operations keep printing their own progress to
 * stdout and their own refusals to stderr, exactly as they do for the CLI
 * verbs.
 *
 * FOLLOW-UPS, also under o->verbose: a statement that succeeds logs,
 * indented beneath its statement line, the work it did beyond what it names
 * -- the part a user cannot see for themselves. Every figure is one the
 * operation computed while doing the work and handed back, never a second
 * look at the image:
 *   `dylib insert` and `dylib delete`: the command inserted or removed and
 *     its ordinal, the renumbering map (old->new), and how many nlist
 *     entries and SET_DYLIB_ORDINAL opcodes -- bind, weak and lazy -- the
 *     renumbering changed (rewrite.h's mr_renumbering). A delete that
 *     matched nothing renumbered nothing and logs none of this.
 *   `fixups set classic`: that an already-classic image passed through, or
 *     the rebases and binds the conversion emitted, the bytes of opcodes and
 *     bytes appended, the commands it stripped, and how far it extended
 *     __LINKEDIT (declassify.h's md_report).
 *   `swift-abi set legacy`: how many class records it retagged, or
 *     "nothing to retag".
 * Every other statement logs only its statement line.
 *
 * DIRECTIVES.
 *
 * allow-grow covers only `dylib` and `rpath` statements: they are the ones
 * whose load commands can outgrow the header pad, and for them it lets the
 * rewrite enlarge the pad (mg_grow_header) instead of refusing.
 * `version-min set` is not covered -- it refuses with "no room for
 * LC_VERSION_MIN_MACOSX" even under allow-grow, because
 * mv_add_version_min_image has no grow path. `segment rename` and
 * `load-command delete` never add bytes to the load commands, so they never
 * need it.
 *
 * fatal-warnings turns a statement that matched nothing from a report into a
 * refusal of the whole run. The statements that can miss are the ones that
 * name something the image must already have: `load-command delete` (no
 * command of that kind), `dylib replace/delete/reexport` and `rpath
 * replace/delete` (no command naming that path), and `segment rename` (no
 * segment of that name). Each miss is reported on stderr as a
 * "macho9: ... matched nothing" line (for load-command delete, "macho9: no
 * load command of kind KIND to delete"); without fatal-warnings that is all
 * that happens and the run continues. `append` and `insert` always act, and
 * the three `set` statements (version-min, swift-abi, fixups) set a state,
 * so for them "already so" or "nothing to retag" is success, never a miss.
 */
int me_run(const char *path, const char *out, const ms_script *s,
           const me_opts *o);

#endif /* MACHO9_EDIT_H */
