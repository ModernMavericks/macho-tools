#include "lc_kinds.h"
#include <mach-o/loader.h>
#include "mach_compat.h"

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

const char *lc_kind_name(uint32_t cmd) {
    for (size_t i = 0; i < LC_STRIP_KINDS_COUNT; i++)
        if (LC_STRIP_KINDS[i].cmd == cmd)
            return LC_STRIP_KINDS[i].name;
    return "unknown";
}
