#ifndef MACHO9_SEGNAME_H
#define MACHO9_SEGNAME_H
/*
 * mseg_ -- renaming a Mach-O segment, and the segname each of its sections
 * repeats.
 *
 * This is compat/rename_segment.c's rs_rename_lc, lifted out of that tool so
 * it is a library function rather than one program's static. cli/macho9.c's
 * `segment` verb and src/edit.c's `segment rename` statement are its only C
 * front-ends, both through src/rewrite.h's mr_ops; the old grammar,
 * `rename_segment binary OLDNAME NEWNAME`, reaches this same code through
 * compat/rename_segment.sh, the /bin/sh wrapper that replaced
 * compat/rename_segment.c. That wrapper is what still reproduces the old
 * tool's observables -- thin only, exit 2 when nothing matched, and one
 * "%s: renamed %d segment(s) %s -> %s" line -- where the verb routes the
 * rename through mr_apply_file (src/rewrite.h) and so gets fat containers and
 * an atomic write-back for free.
 *
 * WHY __DATA_CONST -> __DATA, the case this exists for. 10.9's Objective-C
 * runtime finds an image's metadata by asking for named sections of the
 * __DATA segment -- __objc_imageinfo, __objc_classlist, __objc_catlist,
 * __objc_protolist and the rest. Linkers from Xcode 10 onward place those in
 * __DATA_CONST instead (a segment later dyld versions re-protect read-only
 * once binding is done). 10.9's libobjc does not look there, so it concludes
 * the image contains no Objective-C at all and skips it: classes go
 * unregistered and, more subtly, the __objc_selrefs entries are never fixed
 * up. Each selref then still holds a pointer to its method-name string rather
 * than a registered SEL, and the first message sent through one dies with
 *
 *   NSForwarding: warning: selector (0x...) for message 'foo:' does not match
 *   selector known to Objective C runtime
 *
 * Renaming __DATA_CONST to __DATA puts the sections where the runtime looks.
 * It is safe because the two segments carry the same protections (initprot
 * read+write); __DATA_CONST differs only in that a newer dyld hardens it after
 * fixups, which 10.9's dyld never does either way. The resulting image has two
 * segments named __DATA, which is legal -- section lookup is by the
 * (segment, section) name pair, and no section name appears in both.
 *
 * (This explanation lived in compat/rename_segment.c's header until that file
 * became a shell wrapper; it is here now because it is the reason the code is
 * here, and it must outlive whichever front-end reaches it.)
 */
#include <mach-o/loader.h>

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

#endif /* MACHO9_SEGNAME_H */
