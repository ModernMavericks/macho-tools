/* mach_compat.h -- Mach-O constants newer than the 10.9 SDK's own headers.
 *
 * <mach-o/loader.h> and <mach-o/fat.h> on the 10.9 SDK predate every load
 * command, section type, and 64-bit fat-container constant a modern linker
 * can emit (chained fixups, the export trie, LC_BUILD_VERSION, arm64e/watch-
 * style wide fat offsets, ...). This toolkit still has to recognize them --
 * to refuse chained fixups it doesn't support, to skip a section type it
 * knows is safe, to say "64-bit fat Mach-O; not supported" instead of "not a
 * Mach-O file" -- so it needs their numeric values regardless of which SDK
 * built it. The values themselves are data, not logic, straight from Apple's
 * own <mach-o/loader.h>/<mach-o/fat.h> on a modern SDK; they do not change
 * across compilers or hosts.
 *
 * This used to be six-plus separate copies of the same #ifndef/#define pairs
 * (change_dylib.c, cli/macho9.c, fix_macho.c, patch_macho.c, macho_grow.h --
 * itself carrying two of its own, LC_DYLIB_CODE_SIGN_DRS and
 * S_INIT_FUNC_OFFSETS, in two different places -- and src/ordinals.c,
 * src/lc_kinds.c), at the paths those files had at the time. Most of those
 * files are gone now: change_dylib.c and patch_macho.c were replaced by
 * /bin/sh wrappers around macho9 (compat/<tool>.sh) when the compat tools
 * were retired, fix_macho.c held out in compat/ as C for one more plan and is
 * now a wrapper as well, and macho_grow.h was folded into src/grow.c/
 * src/grow.h (Task 3). Of that list only cli/macho9.c, src/ordinals.c and
 * src/lc_kinds.c still exist. The list is kept as the record of how many
 * places one constant was being spelled in.
 *
 * Each of those copies was guarded so that a real SDK definition always wins,
 * but each was also a place the VALUE could drift from the others if only one
 * copy were ever fixed. One shared header is the fix: every file that still
 * needs these constants includes this one instead of re-declaring its own
 * subset.
 *
 * Guarded with #ifndef exactly as each individual copy was: a build against
 * a modern SDK (or a future 10.9-SDK update that catches up) picks up the
 * real declaration and every one of these is a no-op.
 */

#ifndef MACHO9_MACH_COMPAT_H
#define MACHO9_MACH_COMPAT_H

#include <mach-o/loader.h>

/* ---- load commands -------------------------------------------------- */

#ifndef LC_SOURCE_VERSION
#define LC_SOURCE_VERSION 0x2A
#endif
#ifndef LC_DYLIB_CODE_SIGN_DRS
#define LC_DYLIB_CODE_SIGN_DRS 0x2B
#endif
#ifndef LC_LINKER_OPTION
#define LC_LINKER_OPTION 0x2D
#endif
#ifndef LC_LINKER_OPTIMIZATION_HINT
#define LC_LINKER_OPTIMIZATION_HINT 0x2E
#endif
#ifndef LC_DYLD_ENVIRONMENT
#define LC_DYLD_ENVIRONMENT 0x27
#endif
#ifndef LC_NOTE
#define LC_NOTE 0x31
#endif
#ifndef LC_BUILD_VERSION
#define LC_BUILD_VERSION 0x32
#endif
#ifndef LC_DYLD_EXPORTS_TRIE
#define LC_DYLD_EXPORTS_TRIE 0x80000033
#endif
#ifndef LC_DYLD_CHAINED_FIXUPS
#define LC_DYLD_CHAINED_FIXUPS 0x80000034
#endif
#ifndef LC_FILESET_ENTRY
#define LC_FILESET_ENTRY 0x80000035
#endif
#ifndef LC_ATOM_INFO
#define LC_ATOM_INFO 0x36
#endif

/* LC_LOAD_UPWARD_DYLIB is declared in every SDK's <mach-o/loader.h> that has
 * LC_REQ_DYLD (10.9's does), just not always spelled out as its own macro in
 * older headers -- guarded the same way as everything else here for that
 * reason, computed from LC_REQ_DYLD rather than hand-encoded, so it can never
 * disagree with whatever LC_REQ_DYLD this SDK provides. */
#ifndef LC_LOAD_UPWARD_DYLIB
#define LC_LOAD_UPWARD_DYLIB (0x23 | LC_REQ_DYLD)
#endif

/* LC_LAZY_LOAD_DYLIB: the legacy -lazy_library form. Carries a library
 * ordinal like LC_LOAD_DYLIB does; this codebase refuses rather than
 * renumbers it (see ordinals.c's mo_map_build) because nothing here has
 * tested that renumbering. */
#ifndef LC_LAZY_LOAD_DYLIB
#define LC_LAZY_LOAD_DYLIB 0x20
#endif

/* ---- section types ---------------------------------------------------- */

/* dyld4-era section type: 4-byte initializer offsets FROM THE IMAGE BASE,
 * replacing the absolute pointers of S_MOD_INIT_FUNC_POINTERS. */
#ifndef S_INIT_FUNC_OFFSETS
#define S_INIT_FUNC_OFFSETS 0x16
#endif

/* ---- 64-bit fat container ---------------------------------------------
 *
 * <mach-o/fat.h> on the 10.9 SDK predates the 64-bit fat container (wide
 * offsets, for arm64e/watchOS-style slices with a component that overflows
 * 32 bits) -- so these are not conditional on anything this codebase
 * controls, only on which SDK headers happened to be on the include path.
 */
#ifndef FAT_MAGIC_64
#define FAT_MAGIC_64 0xcafebabfu
#endif
#ifndef FAT_CIGAM_64
#define FAT_CIGAM_64 0xbfbafecau
#endif

#endif /* MACHO9_MACH_COMPAT_H */
