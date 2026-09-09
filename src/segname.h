#ifndef MACHO9_SEGNAME_H
#define MACHO9_SEGNAME_H
/*
 * mseg_ -- renaming a Mach-O segment, and the segname each of its sections
 * repeats.
 *
 * This is compat/rename_segment.c's rs_rename_lc, lifted out of that tool so
 * it is a library function rather than one program's static. Two front-ends
 * call it now and must keep behaving identically: compat/rename_segment.c
 * (the old grammar, `rename_segment binary OLDNAME NEWNAME`, thin-only, its
 * own lseek+write, exit 2 when nothing matched) and cli/macho9.c's `segment`
 * verb, which routes the same rename through mr_apply_file (src/rewrite.h) and
 * so gets fat containers and the atomic write-back for free. Only what
 * genuinely differs between the two -- argument parsing, the write path, the
 * exit code, the message -- stays in each front-end.
 *
 * WHY __DATA_CONST -> __DATA, the case this exists for: 10.9's Objective-C
 * runtime finds an image's metadata by asking for named sections of the
 * __DATA segment. Linkers from Xcode 10 onward place those in __DATA_CONST
 * instead, where 10.9's libobjc never looks; see compat/rename_segment.c's
 * header comment for the full failure mode.
 */
#include <mach-o/loader.h>

#include "image.h"

/* Bytes a segname/sectname field holds. They are NOT NUL-terminated when they
 * use all 16, which is the trap every comparison and copy here wraps. */
#define MSEG_NAME_MAX 16

/* True if `name` fits in a segname field. Both front-ends refuse a longer NEW
 * name before any I/O, and refuse it at the same point, because they ask this
 * same question. */
int mseg_name_fits(const char *name);

/* Rename `lc` if it is an LC_SEGMENT_64 whose segname is `oldname`, giving it
 * -- and the copy of the segment name each of its sections carries -- the name
 * `newname`. Returns 1 if it renamed, 0 if `lc` did not match.
 *
 * Edits segname/sectname CONTENT only: never lc->cmd, never lc->cmdsize. That
 * is what makes it legal as an mi_each_lc callback's work (image.h's "the
 * command-chain shape itself must stay exactly what it was") and what keeps it
 * from desynchronizing that walk's own `p += lc->cmdsize` stride. It is also
 * why mr_build_lcs can call it on a command it has already copied into the new
 * table without re-sizing anything.
 *
 * `lc` is non-const on purpose: this writes through it. */
int mseg_rename_lc(struct load_command *lc, const char *oldname, const char *newname);

/* Rename EVERY matching segment in `im`, in place in its buffer; returns how
 * many were renamed.
 *
 * Every match, not just the first -- a binary that has already been through
 * this rename once (or has duplicate segment names for any other reason) can
 * legitimately have more than one, and each one's sections need the same
 * rename. That is what rules out mi_find_segment, which only ever returns the
 * first match. */
int mseg_rename_image(const mi_image *im, const char *oldname, const char *newname);

#endif /* MACHO9_SEGNAME_H */
