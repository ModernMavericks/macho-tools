#ifndef MACHO9_SWIFT_RETAG_H
#define MACHO9_SWIFT_RETAG_H
/*
 * mswift_ -- moving an Objective-C class record's is-Swift tag from the
 * stable-ABI bit to the legacy one, so a Swift runtime built for a
 * pre-10.14.4 deployment target recognises the class.
 *
 * This is compat/retag_swift_classes.c's whole per-file process(), lifted out
 * of that tool so it is a library function rather than one program's static.
 * cli/macho9.c's `retag-swift` verb and src/edit.c's `swift-abi set legacy`
 * statement are its only C front-ends (the latter through
 * mswift_retag_image, below, the same retag on an image already in memory);
 * the old grammar, `retag_swift_classes binary [binary ...]`, reaches this same code
 * through compat/retag_swift_classes.sh, the /bin/sh wrapper that replaced
 * compat/retag_swift_classes.c -- and that wrapper is what still keeps the
 * multi-file argv loop, the per-file "%s: retagged %d class record(s)" and
 * "total: ..." messages, and the `had_error ? 1 : 0` exit.
 *
 * WHY THE TWO BITS EXIST, and what a mismatch does at runtime. A class
 * record's data word carries a tag in its low two bits saying whether the
 * class is a Swift class, and which bit is used depends on the deployment
 * target of whatever produced it:
 *
 *   bit 1 (value 2)  stable ABI  -- emitted when targeting macOS 10.14.4+
 *   bit 0 (value 1)  legacy      -- emitted when targeting anything older
 *
 * The Swift runtime checks whichever bit its *own* deployment target implies.
 * A runtime built for 10.9 therefore tests bit 0, while an application built
 * for 10.15 tags its classes with bit 1. Nothing rejects the mismatch: the
 * runtime simply concludes that none of the application's classes are Swift
 * classes, treats each as a plain Objective-C class, and takes the
 * ObjC-class-wrapper path in swift_getObjCClassMetadata. For a Swift class
 * that overrides an Objective-C initialiser, that turns super.init() into a
 * call to itself, and the process dies of an infinite recursion long before
 * anything is drawn.
 *
 * Objective-C itself is indifferent: objc masks both bits off before using the
 * pointer, and on 10.9 pure Objective-C classes leave them zero, so moving the
 * tag from one bit to the other changes nothing for the Objective-C runtime.
 *
 * (This explanation lived in compat/retag_swift_classes.c's header until that
 * file became a shell wrapper; it is here now because it is the reason the
 * code is here, and it must outlive whichever front-end reaches it.)
 *
 * THIN ONLY, deliberately: retag_swift_classes never handled a fat container
 * and this task does not change what it does. A caller handed one gets
 * MSWIFT_NOT_MACHO and is expected to say so rather than report a silent
 * success -- which is what the old tool's bare "return 0" looked like from
 * the outside.
 */
#include "image.h"

/* Negative returns from mswift_retag_file. A caller must test for these by
 * name, not with a bare `< 0`: only MSWIFT_ERROR is a failure of the tool
 * itself, and the compat front-end's exit code has always turned on exactly
 * that distinction. */
#define MSWIFT_ERROR      (-1)  /* open/fstat/write failed, or mi_open's own
                                 * open, fstat, read or whole-file malloc did
                                 * (MI_IO_ERROR); already reported */
#define MSWIFT_NOT_MACHO  (-2)  /* not a readable 64-bit Mach-O; NOTHING printed,
                                 * so a front-end that cares must say so itself */
#define MSWIFT_RACED      (-3)  /* `path` named a different inode by the time it
                                 * was validated; already reported, nothing written */

/*
 * Retag every class record reachable from `path`'s __objc_classlist and
 * __objc_nlclslist (in either __DATA or __DATA_CONST), and the metaclass each
 * one's isa points at, writing the file back in place if anything changed.
 *
 * Returns the number of class records retagged (0 if there were none to do),
 * or one of the MSWIFT_* codes above. Nothing is written when the count is 0,
 * and nothing is written on any failure.
 */
int mswift_retag_file(const char *path);

/*
 * mswift_retag_file's retag, without the file: the same walk over the image
 * `im` views (from mi_open or mi_wrap), rewriting tag bits in place in its
 * buffer -- no open, no race guard, no write. mswift_retag_file is this plus
 * those; src/edit.c calls it for `swift-abi set legacy` against the image it
 * writes once, itself, after the last statement.
 *
 * Returns the number of class records retagged, 0 or more; it has no failure
 * of its own and prints nothing. Only tag bits in __DATA's (or
 * __DATA_CONST's) class records change, so im->size does not.
 */
int mswift_retag_image(mi_image *im);

#endif /* MACHO9_SWIFT_RETAG_H */
