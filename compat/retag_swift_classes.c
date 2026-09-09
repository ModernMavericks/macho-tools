/*
 * Retag Objective-C class records from the stable-ABI is-swift bit to the
 * legacy one, so a Swift runtime built for a pre-10.14.4 deployment target
 * recognises them.
 *
 * A class record's data word carries a tag in its low two bits saying whether
 * the class is a Swift class. Which bit is used depends on the deployment
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
 * Both halves of each class pair are retagged -- the class and its metaclass,
 * reached through the class record's isa field.
 *
 * Usage: retag_swift_classes binary [binary ...]
 *
 * The per-file work is mswift_retag_file (src/swift_retag.h), shared with
 * cli/macho9.c's `retag-swift` verb so the two front-ends cannot drift apart
 * about what retagging a class record means. What is left here is only what
 * differs: this tool's multi-file argv loop, its per-file and total messages,
 * and its `had_error ? 1 : 0` exit.
 */
#include <stdio.h>

#include "swift_retag.h"

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "Usage: %s binary [binary ...]\n", argv[0]); return 1; }
    int total = 0;
    int had_error = 0;
    for (int i = 1; i < argc; i++) {
        int n = mswift_retag_file(argv[i]);
        /* MSWIFT_ERROR is a real failure (can't open/stat/write -- already
         * reported via perror/fprintf inside mswift_retag_file), and it is the
         * ONLY code that makes this tool exit nonzero. Before this, only n>0
         * was checked, so a failure was silently indistinguishable from "0
         * classes found" -- the return value was computed and then discarded,
         * and `retag_swift_classes /no/such/file` exited 0 despite `perror`
         * having already printed "No such file or directory" to stderr. That
         * is exactly the shape of silent success docs/PROPOSAL.md's `verify`
         * section exists to rule out.
         *
         * MSWIFT_NOT_MACHO and MSWIFT_RACED are the two benign skips this
         * multi-file loop has always just kept going past: a non-Mach-O
         * argument is not an error here (the old magic-only check ignored one
         * too), and the raced case has already said so on stderr. They are
         * separate codes only so that macho9's SINGLE-file `retag-swift` verb
         * can report them; testing for MSWIFT_ERROR by name rather than for
         * `n < 0` is what keeps this tool's exit code exactly what it was. */
        if (n == MSWIFT_ERROR) { had_error = 1; continue; }
        if (n > 0) { printf("%s: retagged %d class record(s)\n", argv[i], n); total += n; }
    }
    printf("total: %d class record(s) retagged\n", total);
    return had_error ? 1 : 0;
}
