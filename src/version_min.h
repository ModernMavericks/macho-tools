#ifndef MACHO9_VERSION_MIN_H
#define MACHO9_VERSION_MIN_H
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
 * FILE 10.9`: the old name and the verb print exactly the same thing, because
 * there is only one implementation left to print it.
 */
#include "image.h"

/*
 * Append LC_VERSION_MIN_MACOSX 10.9 to the thin 64-bit Mach-O at `path`,
 * writing the result back in place. Returns 0 on success -- including the
 * "already has one, nothing to do" case -- or 1 with a message already
 * printed on stderr.
 *
 * The new command goes in the header pad, so no file data moves and no offset
 * anywhere needs fixing up; if the pad cannot hold it, this refuses rather
 * than resize (use `macho9 grow` / change_dylib -grow first). Fat containers
 * are not handled: add_version_min never did.
 */
int mv_add_version_min(const char *path);

/*
 * mv_add_version_min's edit, without the file: append LC_VERSION_MIN_MACOSX
 * 10.9 to the image `im` views (from mi_open or mi_wrap), in place, and
 * nothing else -- no open, no race guard, no write. mv_add_version_min is
 * this plus those; src/edit.c calls it for `version-min set 10.9` against the
 * image it writes once, itself, after the last statement.
 *
 * Returns 0 with *out_added = 1 if it appended the command, 0 with
 * *out_added = 0 if the image already had one (after printing "already
 * present; nothing to do." on stdout, as mv_add_version_min always has), or
 * MR_REFUSED (src/rewrite.h) with "no room for LC_VERSION_MIN_MACOSX" on
 * stderr when the header pad cannot hold the 16 bytes. The appended command
 * lives in the header pad, so im->size does not change.
 */
int mv_add_version_min_image(mi_image *im, int *out_added);

#endif /* MACHO9_VERSION_MIN_H */
