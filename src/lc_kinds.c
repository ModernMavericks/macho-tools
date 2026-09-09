#include "lc_kinds.h"
#include <mach-o/loader.h>

/* Not declared in every SDK's mach-o/loader.h (10.9's predates them). Same
 * fallback values change_dylib.c, cli/macho9.c, patch_macho.c, fix_macho.c
 * and add_version_min.c already carry for their own purposes -- data, not
 * logic, so one more copy here (to build LC_STRIP_KINDS itself) is the same
 * call this codebase already made elsewhere; consolidating those fallback
 * #defines is tracked separately and out of scope here. */
#ifndef LC_SOURCE_VERSION
#define LC_SOURCE_VERSION 0x2A
#endif
#ifndef LC_BUILD_VERSION
#define LC_BUILD_VERSION 0x32
#endif
#ifndef LC_DYLIB_CODE_SIGN_DRS
#define LC_DYLIB_CODE_SIGN_DRS 0x2B
#endif

/* Load commands safe to drop: purely informational, or invalidated the
 * moment the binary is rewritten. Deliberately excludes LC_FUNCTION_STARTS
 * (avxemu reads it for patch-safety bounds) and LC_DATA_IN_CODE. None of
 * them carries a library ordinal, so stripping never disturbs change_dylib's
 * ordinal renumbering. */
const struct lc_kind LC_STRIP_KINDS[] = {
    { "uuid",           LC_UUID                },
    { "codesig",        LC_CODE_SIGNATURE      },
    { "source-version", LC_SOURCE_VERSION      },
    { "build-version",  LC_BUILD_VERSION       },
    { "code-sign-drs",  LC_DYLIB_CODE_SIGN_DRS },
};
const size_t LC_STRIP_KINDS_COUNT = sizeof(LC_STRIP_KINDS) / sizeof(LC_STRIP_KINDS[0]);
