#ifndef MACHOTOOL_VERSION_MIN_H
#define MACHOTOOL_VERSION_MIN_H
/*
 * mv_ -- declaring a 10.9 deployment floor on an image that has none.
 *
 * patch_macho strips LC_BUILD_VERSION and leaves no platform declaration at
 * all; 10.9's dyld uses that signal for some behaviors (including, possibly,
 * TLV handling). This appends the LC_VERSION_MIN_MACOSX that 10.9 expects.
 *
 * It was compat/add_version_min.c's whole main(). It lives here so that
 * cli/macho9.c's `minos` verb can do the work in-process instead of forking
 * and exec'ing add_version_min -- the same cycle mr_apply_file (src/rewrite.h)
 * breaks for `dylib`/`rpath`/`lc`. That is also what let add_version_min
 * become compat/add_version_min.sh, a /bin/sh wrapper that runs `macho9 minos
 * FILE OUT 10.9` and installs OUT over FILE itself: the old name and the verb
 * print exactly the same thing, because there is only one implementation left
 * to print it.
 */
#include "image.h"

/*
 * Append LC_VERSION_MIN_MACOSX 10.9 to the thin 64-bit Mach-O at `path` and
 * write the result as `out`. `path` is READ and never written; `out` is
 * created afresh (wa_write_new, src/atomic_write.h), carrying `path`'s mode,
 * owner and xattrs. `out` must not be `path` -- wa_write_new refuses that and
 * this returns MR_FAIL without writing anything.
 *
 * Returns 0 on success -- including the "already has one, nothing to do"
 * case, where `out` is still written, so a 0 exit always means `out` exists
 * and is the answer -- or MR_REFUSED/MR_FAIL with a message already printed
 * on stderr. A non-zero return writes no `out` at all.
 *
 * The new command goes in the header pad. If the pad cannot hold it, this
 * refuses -- unless `allow_grow` is set and the image can be grown (a 64-bit
 * PIE executable without chained fixups; see mg_ensure_pad in src/grow.h),
 * in which case the pad is enlarged and `out` grows with it.
 * Fat containers are not handled: add_version_min never did.
 */
int mv_add_version_min(const char *path, const char *out, int allow_grow);

/*
 * mv_add_version_min's edit, without the file: append LC_VERSION_MIN_MACOSX
 * 10.9 to the image in *pbuf, and nothing else -- no open, no race guard, no
 * write. src/edit.c calls it for `version-min set 10.9` against the image it
 * writes once, itself, after the last statement.
 *
 * Returns 0 with *out_added = 1 if it appended the command, 0 with
 * *out_added = 0 if the image already had one (after printing "already
 * present; nothing to do." on stdout, as mv_add_version_min always has), or
 * MR_REFUSED with "no room for LC_VERSION_MIN_MACOSX" on stderr when the
 * command cannot be placed. When the pad is short and `allow_grow` is set,
 * growing it is mg_ensure_pad's decision; if it grows, *pbuf is reallocated,
 * *psize is larger, and every pointer the caller held into the buffer is
 * stale. `label` prefixes mg_ensure_pad's own lines -- its refusal on stderr
 * and, when it grows, "LABEL: load commands need ...; growing header..." and
 * "LABEL: grew header pad: ..." on stdout; both callers pass the file's path.
 */
int mv_add_version_min_image(uint8_t **pbuf, size_t *psize, int allow_grow,
                             const char *label, int *out_added);

#endif /* MACHOTOOL_VERSION_MIN_H */
