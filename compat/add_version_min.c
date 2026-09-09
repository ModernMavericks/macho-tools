/*
 * Append an LC_VERSION_MIN_MACOSX load command targeting 10.9. patch_macho
 * strips LC_BUILD_VERSION and leaves no platform declaration; 10.9's dyld uses
 * that signal for some behaviors (including, possibly, TLV handling).
 *
 * The work itself is mv_add_version_min (src/version_min.h), shared with
 * cli/macho9.c's `minos` verb -- which used to fork and exec THIS binary to
 * get it done, a cycle once add_version_min becomes a wrapper around macho9.
 * All that is left here is the argument check.
 */
#include <stdio.h>

#include "version_min.h"

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "Usage: %s binary\n", argv[0]); return 1; }
    return mv_add_version_min(argv[1]);
}
