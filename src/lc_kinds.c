#include "lc_kinds.h"
#include <string.h>
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

int lc_kind_by_name(const char *name, uint32_t *cmd) {
    for (size_t i = 0; i < LC_STRIP_KINDS_COUNT; i++)
        if (strcmp(name, LC_STRIP_KINDS[i].name) == 0) {
            *cmd = LC_STRIP_KINDS[i].cmd;
            return 0;
        }
    return -1;
}

const char *lc_cmd_name(uint32_t cmd) {
    switch (cmd) {
    case LC_SEGMENT_64:         return "LC_SEGMENT_64";
    case LC_SYMTAB:              return "LC_SYMTAB";
    case LC_DYSYMTAB:            return "LC_DYSYMTAB";
    case LC_LOAD_DYLIB:          return "LC_LOAD_DYLIB";
    case LC_ID_DYLIB:            return "LC_ID_DYLIB";
    case LC_LOAD_WEAK_DYLIB:     return "LC_LOAD_WEAK_DYLIB";
    case LC_REEXPORT_DYLIB:      return "LC_REEXPORT_DYLIB";
    case LC_LOAD_UPWARD_DYLIB:   return "LC_LOAD_UPWARD_DYLIB";
    case LC_RPATH:                return "LC_RPATH";
    case LC_UUID:                 return "LC_UUID";
    case LC_CODE_SIGNATURE:      return "LC_CODE_SIGNATURE";
    case LC_VERSION_MIN_MACOSX:  return "LC_VERSION_MIN_MACOSX";
    case LC_MAIN:                 return "LC_MAIN";
    case LC_DYLD_INFO:            return "LC_DYLD_INFO";
    case LC_DYLD_INFO_ONLY:      return "LC_DYLD_INFO_ONLY";
    case LC_FUNCTION_STARTS:     return "LC_FUNCTION_STARTS";
    case LC_DATA_IN_CODE:        return "LC_DATA_IN_CODE";
    case LC_SOURCE_VERSION:      return "LC_SOURCE_VERSION";
    case LC_BUILD_VERSION:       return "LC_BUILD_VERSION";
    case LC_DYLIB_CODE_SIGN_DRS: return "LC_DYLIB_CODE_SIGN_DRS";
    case LC_DYLD_EXPORTS_TRIE:   return "LC_DYLD_EXPORTS_TRIE";
    case LC_DYLD_CHAINED_FIXUPS: return "LC_DYLD_CHAINED_FIXUPS";
    default:                      return NULL;
    }
}
