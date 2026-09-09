#ifndef MACHO9_SWIFT_RETAG_H
#define MACHO9_SWIFT_RETAG_H
/*
 * mswift_ -- moving an Objective-C class record's is-Swift tag from the
 * stable-ABI bit to the legacy one, so a Swift runtime built for a
 * pre-10.14.4 deployment target recognises the class.
 *
 * This is compat/retag_swift_classes.c's whole per-file process(), lifted out
 * of that tool so it is a library function rather than one program's static.
 * Two front-ends call it now and must keep behaving identically:
 * compat/retag_swift_classes.c (which keeps its multi-file argv loop, its
 * per-file "%s: retagged %d class record(s)" and "total: ..." messages, and
 * its `had_error ? 1 : 0` exit) and cli/macho9.c's `retag-swift` verb. See
 * that tool's header comment for why the two bits exist and what a mismatch
 * does at runtime.
 *
 * THIN ONLY, deliberately: retag_swift_classes never handled a fat container
 * and this task does not change what it does. A caller handed one gets
 * MSWIFT_NOT_MACHO and is expected to say so rather than report a silent
 * success -- which is what the old tool's bare "return 0" looked like from
 * the outside.
 */

/* Negative returns from mswift_retag_file. A caller must test for these by
 * name, not with a bare `< 0`: only MSWIFT_ERROR is a failure of the tool
 * itself, and the compat front-end's exit code has always turned on exactly
 * that distinction. */
#define MSWIFT_ERROR      (-1)  /* open/fstat/write failed; already reported */
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

#endif /* MACHO9_SWIFT_RETAG_H */
